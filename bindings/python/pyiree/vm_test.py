# Lint as: python3
# Copyright 2019 Google LLC
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# pylint: disable=unused-variable

from absl.testing import absltest
import pyiree


def create_simple_mul_module():
  ctx = pyiree.CompilerContext()
  input_module = ctx.parse_asm("""
    func @simple_mul(%arg0: tensor<4xf32>, %arg1: tensor<4xf32>) -> tensor<4xf32>
          attributes { iree.module.export } {
        %0 = "xla_hlo.mul"(%arg0, %arg1) {name = "mul.1"} : (tensor<4xf32>, tensor<4xf32>) -> tensor<4xf32>
        return %0 : tensor<4xf32>
    }
    """)
  binary = input_module.compile()
  m = pyiree.binding.vm.VmModule.from_flatbuffer(binary)
  return m


class VmTest(absltest.TestCase):

  @classmethod
  def setUpClass(cls):
    super().setUpClass()
    driver_names = pyiree.binding.hal.HalDriver.query()
    print("DRIVER_NAMES =", driver_names)
    cls.driver = pyiree.binding.hal.HalDriver.create("vulkan")
    cls.device = cls.driver.create_default_device()
    cls.hal_module = pyiree.binding.vm.create_hal_module(cls.device)

  def test_variant_list(self):
    l = pyiree.binding.vm.VmVariantList(5)
    print(l)
    self.assertEqual(l.size, 0)

  def test_context_id(self):
    instance = pyiree.binding.vm.VmInstance()
    context1 = pyiree.binding.vm.VmContext(instance)
    context2 = pyiree.binding.vm.VmContext(instance)
    self.assertGreater(context2.context_id, context1.context_id)

  def test_module_basics(self):
    m = create_simple_mul_module()
    f = m.lookup_function("simple_mul")
    self.assertGreater(f.ordinal, 0)
    notfound = m.lookup_function("notfound")
    self.assertIs(notfound, None)

  def test_dynamic_module_context(self):
    instance = pyiree.binding.vm.VmInstance()
    context = pyiree.binding.vm.VmContext(instance)
    m = create_simple_mul_module()
    context.register_modules([self.hal_module, m])

  def test_static_module_context(self):
    m = create_simple_mul_module()
    print(m)
    instance = pyiree.binding.vm.VmInstance()
    print(instance)
    context = pyiree.binding.vm.VmContext(
        instance, modules=[self.hal_module, m])
    print(context)


if __name__ == "__main__":
  absltest.main()
