// Dictionary type syntax, static checking, runtime tests and reflection.
#include "fixture.h"

static void test_syntax(void)
{
    LHAT_TEST("an index constraint contains two type expressions");
    Parse p;
    parse_text(&p, "let^ Map = t^{[string^|number^]:t^{[any^]:string^}}\n");
    LHAT_CHECK_EQ_INT(p.result.diagnostic_count, 0);
    parse_dispose(&p);

    const char *bad[] = {
        "let^ T = t^{[]:string^}\n",
        "let^ T = t^{[*]:string^}\n",
        "let^ T = t^{[string^] string^}\n",
        "let^ T = t^{[string^]:}\n",
        "let^ T = t^{[string^]:string^, [number^]:string^}\n",
    };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        LHAT_TEST(bad[i]);
        parse_text(&p, bad[i]);
        LHAT_CHECK(p.result.diagnostic_count > 0, "invalid index constraint");
        parse_dispose(&p);
    }
}

static void test_checking(void)
{
    const char *good[] = {
        "var^ d = {[1 + 1] = \"x\"}\nd[3] := \"y\"\nd := d\n",
        "let^ read = f^ d:t^{[string^]:string^}|t^{[string^]:number^} -> string^|number^|nil^ { return^ d[\"x\"] }\n",
        "let^ read = f^ d:t^{[any^]:string^}&t^{[string^]:any^} -> string^|nil^ { return^ d[\"x\"] }\n",
        "let^ d:t^{[string^]:string^} = {[1] = nil^}\n",
        "let^ d:t^{[string^]:string^} = {a = nil^}\n",
        "let^ a:t^{[string^]:string^} = {}\nlet^ b:t^{[string^]:string^} = {}\n"
        "let^ c = a .. b\nlet^ s:string^|nil^ = c[\"key\"]\n",
        "let^ d:t^{[number^]:string^} = {}\nd.push^(\"value\")\n",
        "let^ d:t^{[string^]:string^} = {}\n"
        "d[\"x\"] := \"ok\"\nlet^ s:string^|nil^ = d[\"x\"]\n"
        "d.x := nil^\nlet^ missing:string^|nil^ = d.x\n",
        "let^ d:t^{[number^]:string^} = {[0] = \"zero\", [-2] = \"minus\", [2.5] = \"half\"}\n"
        "d[100] := \"far\"\nlet^ s:string^|nil^ = d[-2]\n",
        "let^ d:t^{[any^]:string^} = {[true^] = \"yes\", [3] = \"n\", a = \"s\"}\n"
        "d[{}] := \"table key\"\n",
        "let^ d:t^{[string^]:string^} = {[\"1\"] = \"string key\"}\n",
        "let^ k = \"x\"\nlet^ d:t^{[string^]:string^} = {[k] = \"value\"}\n",
        "let^ Key = string^|number^\nlet^ Map = t^{[Key]:string^}\n"
        "let^ read = f^ d:Map, k:Key -> string^|nil^ { return^ d[k] }\n"
        "let^ s = read({a = \"ok\"}, \"a\")\n",
        "let^ d:t^{[string^]:t^{[number^]:string^}} = {row = {[10] = \"x\"}}\n",
        "let^ d:t^{[string^]:string^} = {a = \"x\"}\n"
        "for^ k, v in^ d { let^ key:string^ = k\nlet^ value:string^ = v }\n"
        "for^ k in^ d.keys^() { let^ key:string^ = k }\n"
        "for^ v in^ d.values^() { let^ value:string^ = v }\n",
        "let^ d:t^{[number^]:string^} = {\"a\", \"b\"}\n"
        "for^ v in^ d { let^ value:string^ = v }\n",
        "let^ d:t^{name:string^, [string^]:string^} = {name = \"x\", extra = \"y\"}\n",
        "let^ d:t^{[string^]:string^} = {}\nlet^ c = d.clone^()\nc.x := \"copy\"\n",
        "let^ s:t^{string^[]} = {\"a\", \"b\"}\nlet^ v:string^|nil^ = s[5]\n",
        "let^ D = def^{self^{values:t^{[string^]:string^} = {}},\n"
        "get = f^self^, k:string^ -> string^|nil^ { return^ self^.values[k] },\n"
        "put = p^self^, k:string^, v:string^ { self^.values[k] := v }}\n"
        "let^ d = D.new()\nd.put(\"k\", \"v\")\nlet^ v:string^|nil^ = d.get(\"k\")\n",
    };
    const char *bad[] = {
        "let^ raw = {}\nlet^ fill = p^ d:t^{[string^]:number^} { d.x := 1 }\n"
        "fill(raw)\nlet^ wrong:t^{[string^]:string^} = raw\n",
        "let^ raw = {}\nraw.push^(42)\nlet^ d:t^{[string^]:string^} = raw\n",
        "let^ raw = {}\nlet^ alias:t^{} = raw\nalias.x := 1\nlet^ d:t^{[string^]:string^} = raw\n",
        "let^ d:t^{[string^]:string^} = {a = \"x\"}\n"
        "let^ c = d.clone^(f^ x:any^ -> any^ { return^ 1 })\nlet^ wrong:t^{[string^]:string^} = c\n",
        "let^ write = p^ d:t^{[string^]:string^}|t^{[string^]:number^} { d[\"x\"] := true^ }\n",
        "let^ read = f^ d:t^{[string^]:string^}|t^{[string^]:number^} -> string^|nil^ { return^ d[\"x\"] }\n",
        "let^ d:t^{[string^]:string^} = {}\nd.push^(\"bad numeric key\")\n",
        "let^ d:t^{[number^]:string^} = {}\nd.push^(42)\n",
        "let^ d:t^{[number^]:string^} = {}\nlet^ wrong:t^{number^[]} = {1}\nd.extend^(wrong)\n",
        "let^ d:t^{[string^]:string^} = {}\nlet^ c = d.clone^()\nc.x := 1\n",
        "let^ raw = {}\nraw.x := 1\nlet^ d:t^{[string^]:string^} = raw\n",
        "let^ a:t^{[string^]:string^} = {}\nlet^ b = a .. {wrong = 1}\nlet^ d:t^{[string^]:string^} = b\n",
        "let^ d:t^{[string^]:string^} = {a = 1}\n",
        "let^ d:t^{[string^]:string^} = {\"position\"}\n",
        "let^ d:t^{[number^]:string^} = {a = \"name\"}\n",
        "let^ d:t^{[number^]:string^} = {[\"1\"] = \"string key\"}\n",
        "let^ k = true^\nlet^ d:t^{[string^]:string^} = {[k] = \"v\"}\n",
        "let^ k = \"x\"\nlet^ d:t^{[string^]:string^} = {[k] = 1}\n",
        "let^ d:t^{[string^]:string^} = {}\nd[1] := \"bad key\"\n",
        "let^ d:t^{[string^]:string^} = {}\nd[\"k\"] := 1\n",
        "let^ d:t^{[string^]:string^} = {}\nd.k := true^\n",
        "let^ d:t^{[number^]:string^} = {}\nd.k := \"bad key\"\n",
        "let^ d:t^{[string^]:string^} = {}\nlet^ s = d[true^]\n",
        "let^ d:t^{[string^]:string^} = {}\nlet^ s:string^ = d[\"missing\"]\n",
        "let^ d:t^{[string^]:string^} = {}\nlet^ s:number^|nil^ = d[\"missing\"]\n",
        "let^ take = p^ d:t^{[string^]:string^} {}\ntake({x = 1})\n",
        "let^ d:t^{[number^]:string^} = {}\nlet^ s:t^{[string^]:string^} = d\n",
        "let^ d:t^{[string^]:number^} = {}\nlet^ s:t^{[string^]:string^} = d\n",
        "let^ d:t^{[string^]:t^{[number^]:string^}} = {row = {[10] = 1}}\n",
        "let^ d:t^{[string^]:string^} = {}\n"
        "for^ k, v in^ d { let^ key:number^ = k }\n",
        "let^ T = t^{name:number^, [string^]:string^}\n",
        "let^ T = t^{name:string^, [number^]:string^}\n",
        "let^ f = p^ d:t^{} { let^ typed:t^{[string^]:string^} = d }\n",
    };
    for (int relaxed = 0; relaxed < 2; relaxed++) {
        for (size_t i = 0; i < sizeof good / sizeof *good; i++) {
            LHAT_TEST(good[i]);
            Unit u;
            if (relaxed) check_relaxed_text(&u, good[i]);
            else check_text(&u, good[i]);
            CHECK_CLEAN(&u);
            unit_dispose(&u);
        }
        for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
            LHAT_TEST(bad[i]);
            Unit u;
            if (relaxed) check_relaxed_text(&u, bad[i]);
            else check_text(&u, bad[i]);
            CHECK_REPORTS(&u, LHAT_CHECK_ERR_MISMATCH);
            unit_dispose(&u);
        }
    }
}

static void test_invalid_key(void)
{
    const char *bad[] = {
        "let^ T = t^{[nil^]:string^}\n",
        "let^ d:t^{[any^]:string^} = {}\nd[nil^] := \"invalid\"\n",
    };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        LHAT_TEST(bad[i]);
        Unit u;
        check_text(&u, bad[i]);
        CHECK_REPORTS(&u, LHAT_CHECK_ERR_BAD_KEY);
        unit_dispose(&u);
    }
}

static void test_runtime(void)
{
    struct { const char *text; bool answer; } cases[] = {
        {"return^ {} fits^ t^{[string^]:string^}", true},
        {"return^ {a = {1, 2}} fits^ t^{[string^]:t^{string^[]}}", false},
        {"return^ {a = {1}} fits^ t^{[string^]:t^{number^[2]}}", false},
        {"return^ {a = {1, 2}} fits^ t^{[string^]:t^{number^[2]}}", true},
        {"return^ t^{[string^]:t^{string^[]}} = t^{[string^]:t^{number^[]}}", false},
        {"return^ {a = \"x\"} fits^ t^{[string^]:string^}", true},
        {"return^ {a = 1} fits^ t^{[string^]:string^}", false},
        {"return^ {\"x\"} fits^ t^{[string^]:string^}", false},
        {"return^ {[\"1\"] = \"x\"} fits^ t^{[string^]:string^}", true},
        {"return^ {[\"1\"] = \"x\"} fits^ t^{[number^]:string^}", false},
        {"return^ {[0] = \"x\", [-10] = \"y\", [0.5] = \"z\"} fits^ t^{[number^]:string^}", true},
        {"return^ {[1] = \"x\", [999] = 2} fits^ t^{[number^]:string^}", false},
        {"return^ {[true^] = \"x\", [{}] = \"y\", a = \"z\"} fits^ t^{[any^]:string^}", true},
        {"return^ {a = {b = 1}} fits^ t^{[string^]:t^{[string^]:string^}}", false},
        {"return^ {a = {b = \"x\"}} fits^ t^{[string^]:t^{[string^]:string^}}", true},
        {"return^ {a = nil^} fits^ t^{[string^]:string^}", true},
        {"return^ {a = \"x\"} fits^ t^{a:string^, [string^]:string^}", true},
        {"return^ {} fits^ t^{a:string^, [string^]:string^}", false},
        {"return^ t^{[string^]:string^} = t^{[string^]:number^}", false},
        {"return^ t^{[string^]:string^} = t^{}", false},
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        LHAT_TEST(cases[i].text);
        Run r;
        if (strstr(cases[i].text, "return^ t^") != NULL) {
            run_checked_text(&r, cases[i].text);
        } else {
            run_text(&r, cases[i].text);
        }
        CHECK_BOOL(&r, cases[i].answer);
        run_dispose(&r);
    }
    LHAT_TEST("typed storage, deletion and missing keys");
    Run r;
    run_checked_text(&r,
        "let^ d:t^{[string^]:string^} = {}\n"
        "d.x := \"value\"\nlet^ found = d[\"x\"] ?? \"missing\"\n"
        "d.x := nil^\nreturn^ found .. (d[\"x\"] ?? \" deleted\")\n");
    CHECK_STRING(&r, "value deleted");
    run_dispose(&r);

    LHAT_TEST("dictionary constraints survive typeof and signature writing");
    run_checked_text(&r,
        "let^ d:t^{[string^]:string^} = {}\nreturn^ typeof^(d).signature\n");
    CHECK_STRING(&r, "t^{ [string^] : string^ }");
    run_dispose(&r);

    LHAT_TEST("dictionary values preserve nested sequence signatures");
    run_checked_text(&r,
        "let^ d:t^{[string^]:t^{string^[]}} = {}\nreturn^ typeof^(d).signature\n");
    CHECK_STRING(&r, "t^{ [string^] : t^{ string^[] } }");
    run_dispose(&r);

    LHAT_TEST("a failed cast checks the values of hash entries");
    run_checked_text(&r,
        "let^ d = {a = 1}\n"
        "let^ x = d as^t^{[string^]:string^} catch^ false^\n"
        "return^ x fits^t^{[string^]:string^}\n");
    CHECK_BOOL(&r, false);
    run_dispose(&r);
}

int main(void)
{
    test_syntax();
    test_checking();
    test_invalid_key();
    test_runtime();
    return lhat_test_report("test_dictionary");
}
