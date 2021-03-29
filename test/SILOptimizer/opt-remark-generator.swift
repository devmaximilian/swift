// RUN: %target-swiftc_driver -O -Rpass-missed=sil-opt-remark-gen -Xllvm -sil-disable-pass=FunctionSignatureOpts -emit-sil %s -o /dev/null -Xfrontend -verify
// REQUIRES: optimized_stdlib,swift_stdlib_no_asserts

public class Klass {
    var next: Klass? = nil
}

// TODO: Change global related code to be implicit/autogenerated (as
// appropriate) so we don't emit this remark.
public var global = Klass() // expected-remark {{heap allocated ref of type 'Klass'}}

@inline(never)
public func getGlobal() -> Klass {
    return global // expected-remark @:5 {{retain of type 'Klass'}}
                  // expected-note @-5:12 {{of 'global'}}
                  // expected-remark @-2:12 {{begin exclusive access to value of type 'Klass'}}
                  // expected-note @-7:12 {{of 'global'}}
}

// Make sure that the retain msg is at the beginning of the print and the
// releases are the end of the print.
//
// The heap allocated ref is for the temporary array and the release that is
// unannotated is for that array as well. We make sure that the heap allocated
// ref is on the argument that necessitated its creation.
public func useGlobal() {
    let x = getGlobal()
    print(x) // expected-remark @:11 {{heap allocated ref of type}}
             // expected-remark @-1:12 {{release of type}}
}

public enum TrivialState {
case first
case second
case third
}

struct StructWithOwner {
    var owner = Klass()
    var state = TrivialState.first
}

func printStructWithOwner(x : StructWithOwner) {
    print(x) // expected-remark @:11 {{heap allocated ref of type}}
             // expected-remark @-1 {{retain of type 'Klass'}}
             // expected-note @-3:27 {{of 'x.owner'}}
             // expected-remark @-3:12 {{release of type}}
}

func printStructWithOwnerOwner(x : StructWithOwner) {
    print(x.owner) // expected-remark @:11 {{heap allocated ref of type}}
                   // expected-remark @-1 {{retain of type 'Klass'}}
                   // expected-note @-3:32 {{of 'x.owner'}}
                   // expected-remark @-3:18 {{release of type}}
}

func returnStructWithOwnerOwner(x: StructWithOwner) -> Klass {
    return x.owner // expected-remark {{retain of type 'Klass'}}
                   // expected-note @-2:33 {{of 'x.owner'}}
}

func callingAnInitializerStructWithOwner(x: Klass) -> StructWithOwner {
    return StructWithOwner(owner: x) // expected-remark {{retain of type 'Klass'}}
                                     // expected-note @-2:42 {{of 'x'}}
}

struct KlassPair {
    var lhs: Klass
    var rhs: Klass
}

func printKlassPair(x : KlassPair) {
    // We pattern match columns to ensure we get retain on the p and release on
    // the end ')'
    print(x) // expected-remark @:11 {{heap allocated ref of type}}
             // expected-remark @-1:5 {{retain of type 'Klass'}}
             // expected-note @-5:21 {{of 'x.lhs'}}
             // expected-remark @-3:5 {{retain of type 'Klass'}}
             // expected-note @-7:21 {{of 'x.rhs'}}
             // expected-remark @-5:12 {{release of type}}
}

func printKlassPairLHS(x : KlassPair) {
    // We print the remarks at the 'p' and at the ending ')'.
    print(x.lhs) // expected-remark @:11 {{heap allocated ref of type}}
                 // expected-remark @-1:5 {{retain of type 'Klass'}}
                 // expected-note @-4:24 {{of 'x.lhs'}}
                 // expected-remark @-3:16 {{release of type}}
}

// We put the retain on the return here since it is part of the result
// convention.
func returnKlassPairLHS(x: KlassPair) -> Klass {
    return x.lhs // expected-remark @:5 {{retain of type 'Klass'}}
                 // expected-note @-2:25 {{of 'x.lhs'}}
}

func callingAnInitializerKlassPair(x: Klass, y: Klass) -> KlassPair {
    return KlassPair(lhs: x, rhs: y) // expected-remark {{retain of type 'Klass'}}
                                     // expected-note @-2:36 {{of 'x'}}
                                     // expected-remark @-2:5 {{retain of type 'Klass'}}
                                     // expected-note @-4:46 {{of 'y'}}
}

func printKlassTuplePair(x : (Klass, Klass)) {
    // We pattern match columns to ensure we get retain on the p and release on
    // the end ')'
    print(x) // expected-remark @:11 {{heap allocated ref of type}}
             // expected-remark @-1:5 {{retain of type 'Klass'}}
             // expected-note @-5:26 {{of 'x'}}
             // expected-remark @-3:5 {{retain of type 'Klass'}}
             // expected-note @-7:26 {{of 'x'}}
             // expected-remark @-5:12 {{release of type}}
}

func printKlassTupleLHS(x : (Klass, Klass)) {
    // We print the remarks at the 'p' and at the ending ')'.
    print(x.0) // expected-remark @:11 {{heap allocated ref of type}}
               // expected-remark @-1:5 {{retain of type 'Klass'}}
               // expected-note @-4:25 {{of 'x'}}
               // Release on Array<Any> for print.
               // expected-remark @-4:14 {{release of type}}
}

func returnKlassTupleLHS(x: (Klass, Klass)) -> Klass {
    return x.0 // expected-remark @:5 {{retain of type 'Klass'}}
               // expected-note @-2:26 {{of 'x'}}
}

func callingAnInitializerKlassTuplePair(x: Klass, y: Klass) -> (Klass, Klass) {
    return (x, y) // expected-remark {{retain of type 'Klass'}}
                  // expected-note @-2:41 {{of 'x'}}
                  // expected-remark @-2:5 {{retain of type 'Klass'}}
                  // expected-note @-4:51 {{of 'y'}}
}

public class SubKlass : Klass {
    @inline(never)
    final func doSomething() {}
}

func lookThroughCast(x: SubKlass) -> Klass {
    return x as Klass // expected-remark {{retain of type 'SubKlass'}}
                      // expected-note @-2:22 {{of 'x'}}
}

func lookThroughRefCast(x: Klass) -> SubKlass {
    return x as! SubKlass // expected-remark {{retain of type 'Klass'}}
                          // expected-note @-2:25 {{of 'x'}}
}

func lookThroughEnum(x: Klass?) -> Klass {
    return x! // expected-remark {{retain of type 'Klass'}}
              // expected-note @-2:22 {{of 'x.some'}}
}

func castAsQuestion(x: Klass) -> SubKlass? {
    x as? SubKlass // expected-remark {{retain of type 'Klass'}}
                   // expected-note @-2:21 {{of 'x'}}
}

func castAsQuestionDiamond(x: Klass) -> SubKlass? {
    guard let y = x as? SubKlass else {
        return nil
    }

    y.doSomething()
    return y // expected-remark {{retain of type 'Klass'}}
             // expected-note @-7:28 {{of 'x'}}
}

func castAsQuestionDiamondGEP(x: KlassPair) -> SubKlass? {
    guard let y = x.lhs as? SubKlass else {
        return nil
    }

    y.doSomething()
    // We eliminate the rhs retain/release.
    return y // expected-remark {{retain of type 'Klass'}}
             // expected-note @-8:31 {{of 'x.lhs'}}
}

// We don't handle this test case as well.
func castAsQuestionDiamondGEP2(x: KlassPair) {
    switch (x.lhs as? SubKlass, x.rhs as? SubKlass) { // expected-remark @:39 {{retain of type 'Klass'}}
                                                      // expected-note @-2 {{of 'x.lhs'}}
                                                      // expected-remark @-2:19 {{retain of type 'Klass'}}
                                                      // expected-note @-4 {{of 'x.rhs'}}
    case let (.some(x1), .some(x2)):
        print(x1, x2) // expected-remark @:15 {{heap allocated ref of type}}
                      // expected-remark @-1 {{release of type}}
    case let (.some(x1), nil):
        print(x1) // expected-remark @:15 {{heap allocated ref of type}}
                  // expected-remark @-1 {{release of type}}
    case let (nil, .some(x2)):
        print(x2) // expected-remark @:15 {{heap allocated ref of type}}
                  // expected-remark @-1 {{release of type}}
    case (nil, nil):
        break
    }
}

func inoutKlassPairArgument(x: inout KlassPair) -> Klass {
    return x.lhs // expected-remark {{retain of type 'Klass'}}
                 // expected-note @-2 {{of 'x.lhs'}}
}

func inoutKlassTuplePairArgument(x: inout (Klass, Klass)) -> Klass {
    return x.0 // expected-remark {{retain of type 'Klass'}}
               // expected-note @-2 {{of 'x.0'}}
}

func inoutKlassOptionalArgument(x: inout Klass?) -> Klass {
    return x! // expected-remark {{retain of type 'Klass'}}
              // expected-note @-2 {{of 'x.some'}}
}

func inoutKlassBangCastArgument(x: inout Klass) -> SubKlass {
    return x as! SubKlass // expected-remark {{retain of type 'Klass'}}
                          // expected-note @-2 {{of 'x'}}
}

func inoutKlassQuestionCastArgument(x: inout Klass) -> SubKlass? {
    return x as? SubKlass // expected-remark {{retain of type 'Klass'}}
                          // expected-note @-2 {{of 'x'}}
}

func inoutKlassBangCastArgument2(x: inout Klass?) -> SubKlass {
    return x as! SubKlass // expected-remark {{retain of type 'Klass'}}
                          // expected-note @-2 {{of 'x.some'}}
}

func inoutKlassQuestionCastArgument2(x: inout Klass?) -> SubKlass? {
    return x as? SubKlass // expected-remark {{retain of type 'Klass'}}
                          // expected-note @-2 {{of 'x.some'}}
}

// We should have 1x rr remark here on calleeX for storing it into the array to
// print. Release is from the array. We don't pattern match it due to the actual
// underlying Array type name changing under the hood in between platforms.
@inline(__always)
func alwaysInlineCallee(_ calleeX: Klass) {
    print(calleeX) // expected-remark @:11 {{heap allocated ref of type}}
                   // expected-remark @-1:5 {{retain of type 'Klass'}}
                   // expected-note @-3:27 {{of 'calleeX'}}
                   // expected-remark @-3:18 {{release of type}}
}

// We should have 3x rr remarks here on callerX and none on calleeX.  All of the
// releases are for the temporary array that we pass into print.
//
// TODO: Should we print out as notes the whole inlined call stack?
func alwaysInlineCaller(_ callerX: Klass) {
    alwaysInlineCallee(callerX) // expected-remark @:5 {{heap allocated ref of type}}
                                // expected-remark @-1:5 {{retain of type 'Klass'}}
                                // expected-note @-3:27 {{of 'callerX'}}
                                // expected-remark @-3:31 {{release of type}}
    print(callerX)              // expected-remark @:11 {{heap allocated ref of type}}
                                // expected-remark @-1:5 {{retain of type 'Klass'}}
                                // expected-note @-7:27 {{of 'callerX'}}
                                // expected-remark @-3:18 {{release of type}}
    alwaysInlineCallee(callerX) // expected-remark @:5 {{heap allocated ref of type}}
                                // expected-remark @-1:5 {{retain of type 'Klass'}}
                                // expected-note @-11:27 {{of 'callerX'}}
                                // expected-remark @-3:31 {{release of type}}
}

func allocateValue() {
    // Remark should be on Klass and note should be on k.
    let k = Klass() // expected-remark @:13 {{heap allocated ref of type 'Klass'}}
                    // expected-note @-1:9 {{of 'k'}}
    print(k)        // expected-remark @:11 {{heap allocated ref of type}}
                    // expected-remark @-1:12 {{release of type}}
}

@inline(never)
func simpleInOutUser<T>(_ x: inout T) {
}

func simpleInOut() -> Klass {
    let x = Klass() // expected-remark @:13 {{heap allocated ref of type 'Klass'}}
                    // expected-note @-1:9 {{of 'x'}}
    simpleInOutUser(&x.next) // expected-remark @:5 {{begin exclusive access to value of type 'Optional<Klass>'}}
                             // expected-note @-3:9 {{of 'x.next'}}
                             // expected-remark @-2:28 {{end exclusive access to value of type 'Optional<Klass>'}}
                             // expected-note @-5:9 {{of 'x.next'}}
    return x
}
