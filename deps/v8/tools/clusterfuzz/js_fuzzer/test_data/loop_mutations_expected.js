// Copyright 2025 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Original: differential_fuzz/fake_resource.js
print("I'm a resource.");

// Original: differential_fuzz/fake_resource.js
print("I'm a resource.");

// Original: loop_mutations.js
let __v_0 = 0;
let __v_1 = 0;
for (const __v_7 = 0; __v_7 < 10000; __v_7++) {
  console.log(/* VariableMutator: Replaced __v_7 with __v_7 */__v_7);
  /* VariableOrObjectMutator: Random mutation */
  __v_0 = __v_0;
  console.log(/* VariableMutator: Replaced __v_1 with __v_1 */__v_1);
  /* VariableOrObjectMutator: Random mutation */
  __v_0 = __getRandomObject(572933);
  __v_1 = /* VariableMutator: Replaced __v_7 with __v_0 */__v_0;
  /* VariableOrObjectMutator: Random mutation */
  __v_0 = __v_0;
  /* VariableMutator: Replaced __v_1 with __v_0 */__v_0 = /* VariableMutator: Replaced __v_7 with __v_7 */__v_7;
  /* VariableMutator: Replaced __v_1 with __v_0 */__v_0 = /* VariableMutator: Replaced __v_7 with __v_7 */__v_7;
  /* VariableOrObjectMutator: Random mutation */
  __v_0 = __v_0;
  /* VariableOrObjectMutator: Random mutation */
  __v_1 = __v_1, __callGC(false);
  __v_7 = __v_1;
  /* VariableOrObjectMutator: Random mutation */
  __v_0 = __v_7, __callGC(true);
  __v_7 = __v_1;
  __v_7 = __v_1;
  /* VariableMutator: Replaced __v_5 with __v_7 */__v_7(/* VariableMutator: Replaced __v_7 with __v_7 */__v_7);
}
let __v_2 = 0;
while (true) {
  if (__v_2++ < 10) break;
  console.log(__v_2);
  /* VariableOrObjectMutator: Random mutation */
  __v_1 = __v_0;
  /* VariableOrObjectMutator: Random mutation */
  __v_1 = __v_0;
  __v_0 = __v_0 + __v_2;
  __v_0 = __v_0 + __v_2;
  /* VariableOrObjectMutator: Random mutation */
  __v_0 = __v_1;
  /* VariableOrObjectMutator: Random mutation */
  __v_1 = __v_1;
  __v_0 = __v_0 + __v_2;
}
let __v_3 = 0;
while (__v_3 < 10) {
  if (__v_5()) break;
}
let __v_4 = 0;
let __v_5 = true;
do {
  /* VariableOrObjectMutator: Random mutation */
  __v_2 = __v_2, __callGC(false);
  console.log(/* VariableMutator: Replaced __v_4 with __v_4 */__v_4);
} while (/* VariableMutator: Replaced __v_5 with __v_5 */__v_5);
const __v_6 = 10;
for (const __v_8 = __v_6; __v_8 > 0; __v_8--) {
  console.log(/* VariableMutator: Replaced __v_8 with __v_0 */__v_0);
  console.log(/* VariableMutator: Replaced __v_6 with __v_1 */__v_1);
  /* VariableOrObjectMutator: Random mutation */
  __v_0 = __v_3, __callGC(true);
  if (strange) {
    __v_8 = __v_6;
    __v_8 = __v_6;
    /* VariableOrObjectMutator: Random mutation */
    __v_0 = __v_2;
    /* VariableOrObjectMutator: Random mutation */
    __v_1 = __v_2;
    __v_8 = __v_6;
  }
}
/* VariableOrObjectMutator: Random mutation */
__v_0 = __v_2, __callGC(false);
console.log(/* VariableMutator: Replaced __v_0 with __v_0 */__v_0);
/* VariableOrObjectMutator: Random mutation */
__v_0 = __v_2, __callGC(true);
console.log(/* VariableMutator: Replaced __v_6 with __v_1 */__v_1);
