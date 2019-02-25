# Import the WebIDL module, so we can do isinstance checks and whatnot
import WebIDL

def WebIDLTest(parser, harness):
    # Basic functionality
    threw = False
    try:
        parser.parse("""
            typedef [EnforceRange] long Foo;
            typedef [Clamp] long Bar;
            typedef [TreatNullAs=EmptyString] DOMString Baz;
            dictionary A {
                required [EnforceRange] long a;
                required [Clamp] long b;
                required [TreatNullAs=EmptyString] DOMString c;
                [ChromeOnly, EnforceRange] long d;
                Foo e;
            };
            interface B {
                attribute Foo typedefFoo;
                attribute [EnforceRange] long foo;
                attribute [Clamp] long bar;
                attribute [TreatNullAs=EmptyString] DOMString baz;
                void method([EnforceRange] long foo, [Clamp] long bar,
                            [TreatNullAs=EmptyString] DOMString baz);
                void method2(optional [EnforceRange] long foo, optional [Clamp] long bar,
                             optional [TreatNullAs=EmptyString] DOMString baz);
            };
            interface Setlike {
                setlike<[Clamp] long>;
            };
            interface Maplike {
                maplike<[Clamp] long, [EnforceRange] long>;
            };
            interface Iterable {
                iterable<[Clamp] long, [EnforceRange] long>;
            };
        """)
        results = parser.finish()
    except WebIDL.WebIDLError as e:
        raise e
        threw = True

    harness.ok(not threw, "Should not have thrown on parsing normal")
    if not threw:
        harness.check(results[0].innerType.enforceRange, True, "Foo is [EnforceRange]")
        harness.check(results[1].innerType.clamp, True, "Bar is [Clamp]")
        harness.check(results[2].innerType.treatNullAsEmpty, True, "Baz is [TreatNullAs=EmptyString]")
        A = results[3]
        harness.check(A.members[0].type.enforceRange, True, "A.a is [EnforceRange]")
        harness.check(A.members[1].type.clamp, True, "A.b is [Clamp]")
        harness.check(A.members[2].type.treatNullAsEmpty, True, "A.c is [TreatNullAs=EmptyString]")
        harness.check(A.members[3].type.enforceRange, True, "A.d is [EnforceRange]")
        harness.check(A.members[4].type.enforceRange, True, "A.e is [EnforceRange]")
        B = results[4]
        harness.check(B.members[0].type.enforceRange, True, "B.typedefFoo is [EnforceRange]")
        harness.check(B.members[1].type.enforceRange, True, "B.foo is [EnforceRange]")
        harness.check(B.members[2].type.clamp, True, "B.bar is [Clamp]")
        harness.check(B.members[3].type.treatNullAsEmpty, True, "B.baz is [TreatNullAs=EmptyString]")
        method = B.members[4].signatures()[0][1]
        harness.check(method[0].type.enforceRange, True, "foo argument of method is [EnforceRange]")
        harness.check(method[1].type.clamp, True, "bar argument of method is [Clamp]")
        harness.check(method[2].type.treatNullAsEmpty, True, "baz argument of method is [TreatNullAs=EmptyString]")
        method2 = B.members[5].signatures()[0][1]
        harness.check(method[0].type.enforceRange, True, "foo argument of method2 is [EnforceRange]")
        harness.check(method[1].type.clamp, True, "bar argument of method2 is [Clamp]")
        harness.check(method[2].type.treatNullAsEmpty, True, "baz argument of method2 is [TreatNullAs=EmptyString]")

    ATTRIBUTES = [("Clamp", "long"), ("EnforceRange", "long"), ("TreatNullAs=EmptyString", "DOMString")]
    TEMPLATES = [
        ("required dictionary members", """
            dictionary Foo {
                [%s] required %s foo;
            }
        """, 3),
        ("optional arguments", """
            interface Foo {
                void foo([%s] optional %s foo)
            }
        """, 3),
        ("typedefs", """
            [%s] typedef %s foo;
        """, 3),
        ("attributes", """
            interface Foo {
            [%s] attribute %s foo;
            }
        """, 3),
        ("unions", """
            typedef [%s] (%s or short) Foo;
        """, 2),
        ("unions", """
            typedef [%s] (%s or short) Foo;
        """, 2),
        ("readonly attributes", """
            interface Foo {
                readonly [%s] attribute %s foo;
            }
        """, 2)
    ];

    for (name, template, limit) in TEMPLATES:
        for (attribute, type) in ATTRIBUTES[:limit]:
            threw = False
            try:
                parser.parse(template % (attribute, type))
                results = parser.finish()
            except:
                threw = True

            harness.ok(threw,
                       "Should not allow [%s] on %s" % (attribute, name))
    threw = False
    try:
        parser.parse("""
            typedef [Clamp, EnforceRange] long Foo;
        """)
        parser.finish()
    except:
        threw = True

    harness.ok(threw, "Should not allow mixing [Clamp] and [EnforceRange]")

    threw = False
    try:
        parser.parse("""
            typedef [Clamp] long Foo;
            typedef [EnforceRange] Foo bar;
        """)
        parser.finish()
    except:
        threw = True

    harness.ok(threw, "Should not allow mixing [Clamp] and [EnforceRange] via typedefs")

    threw = False
    try:
        parser.parse("""
            typedef [Clamp] DOMString Foo;
        """)
        parser.finish()
    except:
        threw = True

    harness.ok(threw, "Should not allow [Clamp] on DOMString")


    threw = False
    try:
        parser.parse("""
            typedef [EnforceRange] DOMString Foo;
        """)
        parser.finish()
    except:
        threw = True

    harness.ok(threw, "Should not allow [EnforceRange] on DOMString")


    threw = False
    try:
        parser.parse("""
            typedef [TreatNullAs=DOMString] long Foo;
        """)
        parser.finish()
    except:
        threw = True

    harness.ok(threw, "Should not allow [TreatNullAs] on long")
