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
class TensorArray(Object):
    @property
    def ndim(self):
        return len(self.shape)

    @property
    def shape(self):
        return self.__getattr__("shape")

    @property
    def tensor_shape(self):
        return self.__getattr__("tensor_shape")

    @property
    def name(self):
        return self.__getattr__("name")

def decl_region_tensor_array(shape,
                             tensor_shape,
                             dtype=None,
                             name="region_ta"):
    """Declare a new RegionTensorArray.

    Parameters
    ----------
    shape : tuple of Expr
        The shape of the tensor array.

    tensor_shape : tuple of Expr
        The shape of the tensors in the tensor array.

    dtype : str, optional
        The data type of the buffer.

    name : str, optional
        The name of the buffer.

    Returns
    -------
    region_ta : RegionTensorArray
        The created RegionTensoArray
    """
    data = Var(name, "handle")
    return _ffi_api.RegionTensorArray(
        data, dtype, shape, tensor_shape, name)

def decl_pointer_tensor_array(shape,
                              region_ta,
                              name="pointer_ta"):
    """Declare a new PointerTensorArray.

    Parameters
    ----------
    shape : tuple of Expr
        The shape of the buffer.

    region_ta : RegionTensorArray
        The RegionTensorArray corresponding to this PointerTensorArray.

    name : str, optional
        The name of the buffer.

    Returns
    -------
    pointer_ta : PointerTensorArray
        The created PointerTensoArray
    """
    data = Var(name, "handle")
    return _ffi_api.PointerTensorArray(
        data, region_ta, shape, name)


def lower_tensor_array(tensor_arrays, buffers, input_program, target, config):
    return _ffi_api.lower_tensor_arrays(tensor_arrays, buffers, input_program, target, config)