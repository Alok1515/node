// Copyright 2024 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Flags: --allow-natives-syntax --turbolev --turbofan

eval("function f(b) { return [ " + ",".repeat(4000) + " ...b ] }");

%PrepareFunctionForOptimization(f);
f([1, 2, 3]);
let arr = f([1, 2, 3]);
%OptimizeFunctionOnNextCall(f);
assertEquals(arr, f([1, 2, 3]));
assertOptimized(f);
