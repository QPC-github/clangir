// RUN: %clang_cc1 -std=c++17 -mconstructor-aliases -triple x86_64-unknown-linux-gnu -fclangir-enable -emit-cir %s -o - | FileCheck %s

// TODO: support -mno-constructor-aliases

struct String {
  long size;
  String(const String &s) : size{s.size} {}
// CHECK: cir.func linkonce_odr @_ZN6StringC2ERKS_
// CHECK:     %0 = cir.alloca !cir.ptr<!ty_22struct2EString22>, cir.ptr <!cir.ptr<!ty_22struct2EString22>>, ["this", init] {alignment = 8 : i64}
// CHECK:     %1 = cir.alloca !cir.ptr<!ty_22struct2EString22>, cir.ptr <!cir.ptr<!ty_22struct2EString22>>, ["s", init] {alignment = 8 : i64}
// CHECK:     cir.store %arg0, %0
// CHECK:     cir.store %arg1, %1
// CHECK:     %2 = cir.load %0
// CHECK:     %3 = "cir.struct_element_addr"(%2) {member_name = "size"}
// CHECK:     %4 = cir.load %1
// CHECK:     %5 = "cir.struct_element_addr"(%4) {member_name = "size"}
// CHECK:     %6 = cir.load %5 : cir.ptr <i64>, i64
// CHECK:     cir.store %6, %3 : i64, cir.ptr <i64>
// CHECK:     cir.return
// CHECK:   }

  String() {}
};

void foo() {
  String s;
  String s1{s};
  // FIXME: s1 shouldn't be uninitialized.

  //  cir.func @_Z3foov() {
  //   %0 = cir.alloca !ty_22struct2EString22, cir.ptr <!ty_22struct2EString22>, ["s"] {alignment = 8 : i64}
  //   %1 = cir.alloca !ty_22struct2EString22, cir.ptr <!ty_22struct2EString22>, ["s1"] {alignment = 8 : i64}
  //   cir.call @_ZN6StringC2Ev(%0) : (!cir.ptr<!ty_22struct2EString22>) -> ()
  //   cir.call @_ZN6StringC2ERKS_(%1, %0) : (!cir.ptr<!ty_22struct2EString22>, !cir.ptr<!ty_22struct2EString22>) -> ()
  //   cir.return
  // }
}
