// Copyright 2025 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Flags: --allow-natives-syntax --maglev --no-always-turbofan
// Flags: --typed-array-length-loading

function foo() {
  let a = new Int32Array(100);
  return Math.round(a.length);
}
%PrepareFunctionForOptimization(foo);
assertEquals(100, foo());

%OptimizeMaglevOnNextCall(foo);
assertEquals(100, foo());
