import importlib.util
import pathlib
import shutil
import tempfile
import unittest

SCRIPT = pathlib.Path(__file__).resolve().parents[1] / 'apply_bluetooth_separation.py'
spec = importlib.util.spec_from_file_location('migration', SCRIPT)
migration = importlib.util.module_from_spec(spec)
spec.loader.exec_module(migration)

PHASE1 = pathlib.Path('/mnt/data/firmware_refactor_work/phase1')


class MigrationTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.dir.name)
        def put(path, text):
            p = self.root / path
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text(text)
            return p

        # Phase 3-shaped fixture containing the critical working Phase 1 runtime logic.
        mesh = (PHASE1 / 'components/MeshCorePort/MeshCorePort.cpp').read_text()
        names = {'watch::WatchBLE': 'hardware::bluetooth::Transport', 'watch::WatchSettingsStore': 'meshcore::SettingsStore', 'watch::WatchMesh': 'meshcore::Node', 'watch::WatchCompanion': 'meshcore::Companion', 'watch::WatchTelemetry': 'meshcore::Telemetry', 'watch::WatchContacts': 'meshcore::Contacts', 'watch::WatchClock': 'meshcore::ClockAdapter', 'watch::WatchRTC': 'meshcore::RTCAdapter', 'watch::WatchRNG': 'meshcore::RandomAdapter', 'watch::WatchSettings': 'meshcore::Settings', 'watch::MeshCoreRadio': 'meshcore::RadioAdapter', 'watch::load_or_create_identity': 'meshcore::load_or_create_identity', 'sx1262::': 'hardware::lora::'}
        for a, b in names.items():
            mesh = mesh.replace(a, b)
        mesh = mesh.replace('bootloader_random_enable()', 'hardware::system::enable_boot_random()').replace('bootloader_random_disable()', 'hardware::system::disable_boot_random()')
        mesh = mesh.replace('static hardware::bluetooth::Transport ble;', 'static hardware::bluetooth::Transport ble;')
        uuid = '''    // Original MeshCore NUS/GATT UUIDs, in BLE wire byte order.
    static const hardware::bluetooth::Uuids COMPANION_UUIDS = {
        {0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0, 0x93, 0xF3, 0xA3, 0xB5, 0x01, 0x00, 0x40, 0x6E},
        {0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0, 0x93, 0xF3, 0xA3, 0xB5, 0x02, 0x00, 0x40, 0x6E},
        {0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0, 0x93, 0xF3, 0xA3, 0xB5, 0x03, 0x00, 0x40, 0x6E}
    };
'''
        mesh = mesh.replace('    static const char *TAG = "MeshCorePort";', '    static const char *TAG = "MeshCorePort";\n' + uuid)
        mesh = mesh.replace('ble.begin("MeshCore-Watch")', 'ble.begin("MeshCore-Watch", COMPANION_UUIDS)')
        put('meshcore/MeshCorePort/MeshCorePort.cpp', mesh)
        header = (PHASE1 / 'components/MeshCorePort/include/MeshCorePort.h').read_text()
        put('meshcore/MeshCorePort/include/MeshCorePort.h', header)

        main = (PHASE1 / 'main/main.cpp').read_text()
        main = main.replace('#include "WatchDisplay.h"', '#include "DeviceUI.h"')
        main = main.replace('watch::pairing_display_clear()', 'app::ui::clear_pairing_code()').replace('watch::pairing_display_show(', 'app::ui::show_pairing_code(').replace('watch::pairing_display_init()', 'app::ui::init()')
        put('main/main.cpp', main)
        put('main/CMakeLists.txt', 'idf_component_register(SRCS "main.cpp" "DeviceUI.cpp" REQUIRES\n        MeshCorePort\n        CO5300\n)\n')
        put('meshcore/MeshCorePort/companion/Companion.cpp', 'namespace meshcore {\n    void Companion::update(const Settings &next){\n        ble_.set_pairing_pin(next.ble_pin);\n    }\n    void Companion::loop(){\n        ble_.loop();\n    }\n}\n')
        put('hardware/Bluetooth/include/BluetoothTransport.h', '''#pragma once
namespace hardware::bluetooth {
    struct Uuids { uint8_t service[16]; uint8_t rx[16]; uint8_t tx[16]; };
    class Transport {
    public:
        bool take_clear_pairing_display();
    };
}
''')
        put('hardware/Bluetooth/BluetoothTransport.cpp', '''#include "BluetoothTransport.h"
namespace hardware::bluetooth {
    Transport *instance = nullptr;
    Transport::Transport(){}
    bool Transport::take_clear_pairing_display(){ return true; }
    void security(){
        ble_hs_cfg.sm_io_cap = BLE_HS_IO_DISPLAY_ONLY;
        ble_hs_cfg.sm_bonding = 1;
        ble_hs_cfg.sm_mitm = 1;
        ble_hs_cfg.sm_sc = 1;
    }
}
''')

    def tearDown(self):
        self.dir.cleanup()

    def test_plan_preserves_identity_and_ble_security(self):
        edits = migration.plan(self.root)
        main = edits[self.root / 'main/main.cpp']
        core = edits[self.root / 'meshcore/MeshCorePort/MeshCorePort.cpp']
        self.assertEqual(main.count('0x9E, 0xCA'), 3)
        self.assertEqual(core.count('0x9E, 0xCA'), 0)
        self.assertIn('load_or_create_identity', core)
        self.assertIn('set_position', core)
        self.assertIn('meshcore::loop();', main)
        self.assertIn('bluetooth.set_pairing_pin(requested_ble_pin);', main)
        self.assertIn('bluetooth.loop();', main)
        self.assertNotIn('app::ui::init()', main)
        self.assertNotIn('ble.begin(', core)
        self.assertNotIn('ble_.set_pairing_pin', edits[self.root / 'meshcore/MeshCorePort/companion/Companion.cpp'])

    def test_backup_and_apply(self):
        edits = migration.plan(self.root)
        originals = {path: path.read_bytes() for path in edits}
        backup = migration.apply(self.root, edits)
        self.assertTrue(backup.exists())
        self.assertIn('Transport &transport()', (self.root / 'hardware/Bluetooth/BluetoothTransport.cpp').read_text())
        import tarfile
        with tarfile.open(backup) as tf:
            self.assertEqual(len(tf.getmembers()), 7)
            for member in tf.getmembers():
                self.assertEqual(tf.extractfile(member).read(), originals[self.root / member.name])
        with self.assertRaises(migration.Mismatch):
            migration.plan(self.root)

    def test_mismatch_does_not_modify(self):
        path = self.root / 'meshcore/MeshCorePort/MeshCorePort.cpp'
        path.write_text(path.read_text().replace('COMPANION_UUIDS', 'BAD_UUIDS'))
        before = path.read_text()
        with self.assertRaises(migration.Mismatch):
            migration.plan(self.root)
        self.assertEqual(path.read_text(), before)

    def test_verify_after_apply(self):
        edits = migration.plan(self.root)
        migration.apply(self.root, edits)
        migration.verify_installed(self.root)

    def test_check_only_does_not_write(self):
        before = {p: p.read_bytes() for p in migration.plan(self.root)}
        _ = migration.plan(self.root)
        self.assertEqual(before, {p: p.read_bytes() for p in before})

    def test_real_nimble_display_only_constant(self):
        edits = migration.plan(self.root)
        ble_source = edits[self.root / 'hardware/Bluetooth/BluetoothTransport.cpp']
        self.assertIn('BLE_HS_IO_DISPLAY_ONLY', ble_source)

    def test_missing_display_only_has_diagnostic(self):
        ble_file = self.root / 'hardware/Bluetooth/BluetoothTransport.cpp'
        ble_file.write_text(ble_file.read_text().replace('BLE_HS_IO_DISPLAY_ONLY', 'BLE_HS_IO_NO_INPUT_OUTPUT'))
        with self.assertRaisesRegex(migration.Mismatch, 'DISPLAY_ONLY'):
            migration.plan(self.root)

    def test_bluetooth_singleton_header_has_forward_decl(self):
        edits = migration.plan(self.root)
        header = edits[self.root / 'hardware/Bluetooth/include/BluetoothTransport.h']
        self.assertIn('class Transport;\n    Transport &transport();', header)

if __name__ == '__main__':
    unittest.main()
