# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.
"""Expression AST Node in TVM.

User do not need to deal with expression AST node directly.
But they can be helpful for developer to do quick proptyping.
While not displayed in the document and python file.
Each expression node have subfields that can be visited from python side.

For example, you can use addexp.a to get the left operand of an Add node.

.. code-block:: python

  x = tvm.var("n")
  y = x + 2
  assert(isinstance(y, tvm.tir.Add))
  assert(y.a == x)
"""
import tvm._ffi

from tvm.runtime import Object, ObjectGeneric, DataType, TypeCode, const
from tvm.ir import PrimExpr
import tvm.ir._ffi_api
from . import generic as _generic
from . import _ffi_api
from .expr import UninterpFun

@tvm._ffi.register_object("tir.Modes")
class Modes(tvm.runtime.Object):
    def __init__(self, dims, shape):
        self.__init_handle_by_constructor__(_ffi_api.Modes, dims, shape, [], [])

    def __init__(self, dims, dense_shape, width_ufs, position_ufs, loop_layout = False):
        self.__init_handle_by_constructor__(_ffi_api.Modes, dims, dense_shape, width_ufs, position_ufs, loop_layout)

    def dense_shape(self):
        return _ffi_api.ModesDenseShape(self)
