"""Controller patches must never write through to the controlled SDK checkout."""
import importlib.util
from pathlib import Path
import shutil
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
PATH = ROOT / 'tools/bridge_runtime/generate_runtime_bundle.py'
spec = importlib.util.spec_from_file_location('controller_overlay_generator', PATH)
generator = importlib.util.module_from_spec(spec)
spec.loader.exec_module(generator)


class ControllerOverlayTest(unittest.TestCase):
    def test_controller_is_private_and_other_bluetooth_is_linked(self):
        with tempfile.TemporaryDirectory() as directory:
            sdk = Path(directory)
            trees = ['.git', 'components/net/lwip', 'components/os', 'components/mm',
                     'components/fs', 'components/fota', 'components/crypto/mbedtls',
                     'components/wireless/macsw',
                     'components/wireless/bluetooth/btblecontroller/ip/ll',
                     'components/wireless/bluetooth/blestack']
            trees += ['components/wireless/wifi6/'+name for name in
                      ('fhost', 'fhost_config', 'wpa_supplicant', 'macsw_os_adapter')]
            for name in trees: (sdk / name).mkdir(parents=True, exist_ok=True)
            relative = Path('components/wireless/bluetooth/btblecontroller/ip/ll/controller.c')
            (sdk / relative).write_text('original\n')
            config = Path('components/wireless/wifi6/fhost_config/fhost_config_buffer.c')
            (sdk / config).write_text('original config\n')
            overlay = generator.create_patched_sdk_overlay(sdk, patch_records=[])
            try:
                (overlay / config).write_text('private config\n')
                self.assertEqual((sdk / config).read_text(), 'original config\n')
                target = overlay / relative
                self.assertNotEqual(target.resolve(), (sdk / relative).resolve())
                target.write_text('patched\n')
                self.assertEqual((sdk / relative).read_text(), 'original\n')
                self.assertEqual((overlay / 'components/wireless/bluetooth/blestack').resolve(),
                                 (sdk / 'components/wireless/bluetooth/blestack').resolve())
            finally:
                shutil.rmtree(overlay)


if __name__ == '__main__': unittest.main()
