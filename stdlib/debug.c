// L^ (lhat) -- sample standard library: std.debug.
//
// 02 の 10.6 の脱法関数。std.io.print は p^ なので f^ の中からは呼べず、
// 10.6 が言う通り純粋関数の中で起きたことは外から一切観測できない --
// デバッグ中はそれが困る。std.debug.log はその一点のために f^ として
// 登録された、書き出す f^ である。
//
// 穴ではなく例外である理由は 10.6 と、cli/main.c の print が既に立って
// いる根拠と同じ: これが変えるのはホストの状態であって機械の状態ではなく、
// 呼んだ後に L^ のプログラムが読めるもので「何もしなかった場合」と区別
// できるものは何もない。13.2 が f^ にあらゆる経路で答えることを求めるので
// 答えは nil^ -- 04 の 11.3 が既に「ここには何もない」と綴った値である。
//
// 10.6 が「抜け道のために言語機能を設計するのは筋が悪い」として f^ 内の
// finally^ を認めなかったことは、この関数があっても変わらない。これは
// 言語機能ではなくホストが登録する1つの名前であり、要らない host は
// lhatstdlib_debug_register を呼ばなければ純粋性の保証がそのまま戻る。

#include "debug.h"

#include <stdio.h>
#include <stdlib.h>  // malloc for the traceback text

// 一切状態を持たないので context は NULL -- io/thread/random が
// LhatErrorKind や LhatHostDataTag を持ち回るために module 構造体を
// 確保しているのに対し、この モジュールは登録しても何も割り当てない。
static LhatValue debug_log(LhatMachine *machine, void *context,
                           const LhatValue *arguments, size_t count)
{
    (void)machine;
    (void)context;
    (void)count;
    if (lhat_is_object_kind(arguments[0], LHAT_OBJECT_STRING)) {
        const LhatString *text =
            (const LhatString *)lhat_as_object(arguments[0]);
        fwrite(text->text, 1, text->length, stdout);
        fputc('\n', stdout);
    }
    return lhat_nil();
}

// 04 の 11.6改: where the caller stands, as the text a log line wants. A
// host function runs with the frames of its callers still on the machine,
// so this is lhat_machine_traceback over the live stack -- reading it
// changes nothing the program can observe, which is what lets it be an f^
// like log above.
static LhatValue debug_traceback(LhatMachine *machine, void *context,
                                 const LhatValue *arguments, size_t count)
{
    (void)context;
    (void)arguments;
    (void)count;
    size_t needed = lhat_machine_traceback(machine, NULL, 0);
    char *text = (char *)malloc(needed + 1);
    if (text == NULL) {
        return lhat_nil();
    }
    lhat_machine_traceback(machine, text, needed + 1);
    LhatValue out = lhat_nil();
    bool made = lhat_machine_make_string(machine, text, needed, &out);
    free(text);
    return made ? out : lhat_nil();
}

bool lhatstdlib_debug_register(LhatProgram *program)
{
    // std.error にも他のどのモジュールにも依存しない -- 書き出すだけの
    // f^ に失敗する経路がないので、返す誤りもない。
    return lhat_register_func(program, "std.debug", "log",
                              "f^string^ -> nil^;", debug_log, NULL) &&
           lhat_register_func(program, "std.debug", "traceback",
                              "f^ -> string^;", debug_traceback, NULL);
}
