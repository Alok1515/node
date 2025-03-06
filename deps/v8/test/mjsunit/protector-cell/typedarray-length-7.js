// Copyright 2025 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Flags: --allow-natives-syntax

assertTrue(%TypedArrayLengthProtector());

let a = new Uint16Array(20);
assertEquals(20, a.length);

a.__defineSetter__('length', function () {});

assertFalse(%TypedArrayLengthProtector());

assertEquals(undefined, a.length);
