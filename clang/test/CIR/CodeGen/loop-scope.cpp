// RUN: %clang_cc1 -std=c++17 -triple x86_64-unknown-linux-gnu -fclangir-enable -emit-cir %s -o %t.cpp.cir
// RUN: FileCheck --input-file=%t.cpp.cir %s --check-prefix=CPPSCOPE
// RUN: %clang_cc1 -x c -std=c11 -triple x86_64-unknown-linux-gnu -fclangir-enable -emit-cir %s -o %t.c.cir
// RUN: FileCheck --input-file=%t.c.cir %s --check-prefix=CSCOPE

void l0() {
  for (int i = 0;;) {
    int j = 0;
  }
}

// CPPSCOPE: cir.func @_Z2l0v() {
// CPPSCOPE-NEXT:   cir.scope {
// CPPSCOPE-NEXT:     %0 = cir.alloca i32, cir.ptr <i32>, ["i", init] {alignment = 4 : i64}
// CPPSCOPE-NEXT:     %1 = cir.alloca i32, cir.ptr <i32>, ["j", init] {alignment = 4 : i64}
// CPPSCOPE-NEXT:     %2 = cir.const(0 : i32) : i32
// CPPSCOPE-NEXT:     cir.store %2, %0 : i32, cir.ptr <i32>
// CPPSCOPE-NEXT:     cir.loop for(cond :  {

// CSCOPE: cir.func @l0() {
// CSCOPE-NEXT: cir.scope {
// CSCOPE-NEXT:   %0 = cir.alloca i32, cir.ptr <i32>, ["i", init] {alignment = 4 : i64}
// CSCOPE-NEXT:   %1 = cir.const(0 : i32) : i32
// CSCOPE-NEXT:   cir.store %1, %0 : i32, cir.ptr <i32>
// CSCOPE-NEXT:   cir.loop for(cond :  {

// CSCOPE:        })  {
// CSCOPE-NEXT:     cir.scope {
// CSCOPE-NEXT:       %2 = cir.alloca i32, cir.ptr <i32>, ["j", init] {alignment = 4 : i64}
