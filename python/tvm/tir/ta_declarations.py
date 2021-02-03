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
"""Abstraction for array data structures."""
from numbers import Integral
import tvm._ffi

from tvm._ffi.base import string_types
from tvm.runtime import Object, convert
from tvm.ir import PrimExpr
from . import _ffi_api
from .expr import Var

@tvm._ffi.register_object
class TALayout(Object):
    pass

def create_ta_layout(scope, layout):
    return _ffi_api.CreateTALayout(scope, layout)

@tvm._ffi.register_object
class TADeclarations(Object):
    def get_tensor_array(self, name):
        return _ffi_api.TADeclarationsGetTensorArray(self, name)

    def add_ta_layouts(self, ta_layouts):
        return _ffi_api.TADeclarationsAddTALayouts(self, ta_layouts)

def create_ta_declarations(tas, buffers):
    return _ffi_api.CreateTADeclarations(tas, buffers)