// Copyright 2024 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

function var_5(){
    let var_1;
    class var_3 {
        static '' = new class {
            '' = var_1.trigger_error();
            [class {
                [this];
            }];
        };
    }
}

assertThrows(var_5);
