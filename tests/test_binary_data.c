#include "lhat/config.h"
#include "testutil.h"
#include "lhat.h"
#include <string.h>
#if LHAT_WITH_FRONTEND
#include "stdlibutil.h"
#endif
#include "../stdlib/binary.h"
#include "../stdlib/stream.h"
#include <math.h>

#if LHAT_WITH_FRONTEND
static const LhatTestRegister regs[] = {lhatstdlib_stream_register};
static void expect(const char *source, int64_t answer)
{
    LhatTestRan ran = lhat_test_run(regs, 1, source);
    LHAT_CHECK_RAN_INTEGER(ran, answer);
    lhat_test_ran_dispose(&ran);
}
#define IMPORT "import^ std.binary\nimport^ std.stream\nlet^ b=std.binary\nlet^ s=std.stream\n"

static void scripts(void)
{
    LHAT_TEST("schema-free binary tables retain nested arrays, dictionaries and byte strings");
    expect(IMPORT
        "let^ v={1,nil^,3, name='abc', nested={flag=true^}, [false^]='no', [2.5]='fraction'}\n"
        "let^ r=try^b.decode(try^b.encode(v))\n"
        "if^r fits^t^{name:string^,nested:t^{flag:bool^}} and^r.length^=3 and^ r[1]=nil^ and^ r.name='abc' and^ r.nested.flag and^ "
        "r[false^]='no' and^r[2.5]='fraction' {return^1}\nreturn^0\n", 1);

    LHAT_TEST("memory streams support multiple records and remain open after serialization");
    expect(IMPORT
        "let^ w=try^s.writer()\ntry^b.serialize(w,{n=1})\ntry^b.serialize(w,{n=2})\n"
        "let^ r=try^s.reader(try^w.toString())\n"
        "let^ a=try^b.deserialize(r)\n"
        "let^ c=try^b.deserialize(r)\nif^a fits^t^{n:number^} and^c fits^t^{n:number^} {return^ a.n*10+c.n}\nreturn^0\n", 12);

    LHAT_TEST("script wrappers satisfy Reader/Writer contracts without inheriting a stream class");
    expect(IMPORT
        "let^ w=try^s.writer()\nvar^ calls=0\n"
        "let^ wrapper={write=p^self^, buffer:std.binary.Bytes -> nil^|error^ {\n"
        " calls+=1\n L^.gc.collect()\n return^w.write(buffer) }}\n"
        "try^b.serialize(wrapper,{payload={1,2,3}})\n"
        "let^ input=try^s.reader(try^w.toString())\n"
        "let^ reader={read=p^self^, buffer:std.binary.Bytes -> number^|error^ {\n"
        " L^.gc.collect()\n return^input.read(buffer) }}\n"
        "let^ result=try^b.deserialize(reader)\nif^result fits^t^{payload:t^{number^[]}} {return^ result.payload[2]}\nreturn^0\n", 3);

    LHAT_TEST("stream callback errors are returned without losing their identity");
    expect(IMPORT
        "errordef^ E {Failure}\nlet^ e=new^E.Failure{}\n"
        "let^ w={write=p^self^, buffer:std.binary.Bytes -> nil^|error^ {return^e}}\n"
        "let^ result=b.serialize(w,{1})\nif^result is^e {return^1}\nreturn^0\n", 1);

    LHAT_TEST("cycles and instances are refused, but shared data children can be copied");
    expect(IMPORT
        "let^ cycle={}\ncycle['self']:=cycle\nlet^ bad=b.encode(cycle)\n"
        "if^ !(bad fits^std.binary.DataError.Cycle) {return^0}\n"
        "let^ D=def^{self^{n=1}}\nlet^ no=b.encode(D.new())\n"
        "if^ !(no fits^std.binary.DataError.Unsupported) {return^0}\n"
        "let^ child={1}\nlet^ result=try^b.decode(try^b.encode({a=child,b=child}))\n"
        "if^result fits^t^{a:t^{},b:t^{}} and^ result.a[0]=1 and^ !(result.a is^result.b) {return^1}\nreturn^0\n", 1);

    LHAT_TEST("malformed, truncated and trailing bytes produce data errors");
    expect(IMPORT
        "let^ encoded=try^b.encode({42})\n"
        "let^ short=b.decode('')\nlet^ bad=b.decode('not a record')\n"
        "let^ extra=b.decode(encoded::'x')\n"
        "if^short fits^std.binary.DataError.Truncated and^ bad fits^std.binary.DataError.Malformed "
        "and^ extra fits^std.binary.DataError.Malformed {return^1}\nreturn^0\n", 1);

    LHAT_TEST("Bytes resize and disposed memory streams follow the byte contract");
    expect(IMPORT
        "let^ reader=try^s.reader('abc')\nlet^ buffer=b.bytes()\nbuffer.resize(2)\n"
        "let^ a=try^reader.read(buffer)\nlet^ first=buffer.toString()\n"
        "buffer.resize(2)\nlet^ c=try^reader.read(buffer)\n"
        "if^a=2 and^ c=1 and^ first='ab' and^ buffer.toString()='c' {return^1}\nreturn^0\n", 1);

    LHAT_TEST("large tables cross frame boundaries");
    expect(IMPORT
        "let^ data={}\nfor^i from^0 to^4999 {data.push^(i)}\n"
        "let^ w=try^s.writer()\ntry^b.serialize(w,data)\n"
        "let^ r=try^s.reader(try^w.toString())\nlet^ got=try^b.deserialize(r)\n"
        "if^got fits^t^{number^[]} {return^ got.length^ + got[4999]}\nreturn^0\n", 9999);

    LHAT_TEST("disposed streams fail and read callback errors propagate");
    expect(IMPORT
        "let^ w=try^s.writer()\nw.dispose()\n"
        "let^ status=b.serialize(w,{1})\n"
        "if^ !(status fits^std.stream.Error.Disposed) {return^0}\n"
        "errordef^ E {Failure}\nlet^ e=new^E.Failure{}\n"
        "let^ r={read=p^self^,buffer:std.binary.Bytes -> number^|error^ {return^e}}\n"
        "if^b.deserialize(r) is^e {return^1}\nreturn^0\n", 1);
}

#endif

typedef struct { uint8_t data[1024]; size_t size, at; LhatMachine *machine; } Buffer;
static bool write_buffer(void *context, const uint8_t *data, size_t n)
{
    Buffer *b=context;
    if (n>sizeof b->data-b->size) return false;
    memcpy(b->data+b->size,data,n); b->size+=n; return true;
}
static bool read_buffer(void *context, uint8_t *data, size_t capacity, size_t *length)
{
    Buffer *b=context;
    // Deliberately short reads, with collection at every callback.
    lhat_machine_gc_collect(b->machine);
    *length=b->at<b->size && capacity>0 ? 1 : 0;
    if (*length) data[0]=b->data[b->at++];
    return true;
}

static void native(void)
{
    LhatMachine *machine=lhat_machine_new();
    LhatValue table, child, key, text;
    bool refused=false;
    LHAT_CHECK(lhat_machine_make_table(machine,&table),"table");
    LHAT_CHECK(lhat_machine_make_table(machine,&child),"child");
    LHAT_CHECK(lhat_machine_make_string(machine,"child",5,&key),"key");
    LHAT_CHECK(lhat_machine_make_string(machine,"a\0\xff",3,&text),"text");
    lhat_machine_table_set(machine,(LhatTable*)lhat_as_object(child),lhat_integer(0),text,&refused);
    lhat_machine_table_set(machine,(LhatTable*)lhat_as_object(table),key,child,&refused);
    Buffer b={0}; b.machine=machine;
    LHAT_TEST("public binary codec survives short reads and GC without a program or front end");
    LHAT_CHECK_EQ_INT(lhatstdlib_binary_serialize(machine,table,write_buffer,&b,NULL),LHAT_BINARY_DATA_OK);
    LhatValue got=lhat_nil();
    LHAT_CHECK_EQ_INT(lhatstdlib_binary_deserialize(machine,read_buffer,&b,NULL,&got),LHAT_BINARY_DATA_OK);
    LhatValue nested=lhat_table_get_bytes((LhatTable*)lhat_as_object(got),"child",5);
    LhatValue v=lhat_table_get((LhatTable*)lhat_as_object(nested),lhat_integer(0));
    LHAT_CHECK(lhat_is_object_kind(v,LHAT_OBJECT_STRING),"string");
    if (lhat_is_object_kind(v,LHAT_OBJECT_STRING)) {
        const LhatString *s=(const LhatString*)lhat_as_object(v);
        LHAT_CHECK(s->length == 3 && memcmp(s->text,"a\0\xff",3)==0,"raw bytes preserved");
    }
    LHAT_TEST("host-configured read budgets reject counts before allocation");
    LhatBinaryDataLimits limits={sizeof b.data,0,96}; b.at=0;
    LHAT_CHECK_EQ_INT(lhatstdlib_binary_deserialize(machine,read_buffer,&b,&limits,&got),LHAT_BINARY_DATA_LIMIT);
    LHAT_CHECK(lhat_is_nil(got),"no partial result");

    LHAT_TEST("the empty table has stable versioned portable bytes");
    LhatValue empty;
    lhat_machine_make_table(machine,&empty); b.size=b.at=0;
    LHAT_CHECK_EQ_INT(lhatstdlib_binary_serialize(machine,empty,write_buffer,&b,NULL),LHAT_BINARY_DATA_OK);
    static const uint8_t golden[]={0x89,'L','H','T',1,13,10,26,3,0,0,0,6,0,0,0,0,0,0};
    LHAT_CHECK(b.size==sizeof golden && memcmp(b.data,golden,sizeof golden)==0,"wire bytes");

    LHAT_TEST("every truncated prefix is rejected and releases partial roots");
    size_t total=b.size;
    for(size_t i=0;i<total;i++) {
        b.at=0; b.size=i;
        LHAT_CHECK(lhatstdlib_binary_deserialize(machine,read_buffer,&b,NULL,&got)!=LHAT_BINARY_DATA_OK,"prefix %zu",i);
        LHAT_CHECK(lhat_is_nil(got),"no partial result");
    }
    b.size=total;
    LHAT_TEST("oversized frames and unknown payload tags are malformed");
    b.data[8]=255; b.data[9]=255; b.at=0;
    LHAT_CHECK_EQ_INT(lhatstdlib_binary_deserialize(machine,read_buffer,&b,NULL,&got),LHAT_BINARY_DATA_MALFORMED);
    memcpy(b.data,golden,sizeof golden); b.data[12]=255; b.at=0;
    LHAT_CHECK_EQ_INT(lhatstdlib_binary_deserialize(machine,read_buffer,&b,NULL,&got),LHAT_BINARY_DATA_MALFORMED);
    lhat_machine_dispose(machine);
}

int main(void)
{
#if LHAT_WITH_FRONTEND
    scripts();
#endif
    native(); return lhat_test_report("test_binary_data");
}
