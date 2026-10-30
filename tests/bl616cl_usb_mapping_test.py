from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch, Mock
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
import bl616cl_usb_mapping as mapping
from bl616cl_prepared_usb_attach import PreparedBlAttach
class MappingTest(unittest.TestCase):
    def setUp(self):
        bus=patch.object(mapping,'dedicated_bus_id',return_value='4-1')
        bus.start();self.addCleanup(bus.stop)

    def test_prepared_process_requires_completed_native_marker(self):
        for code, output, success in [(0,b'BL616CL_PREPARED_ATTACH_ENTER\nBL616CL_PREPARED_ATTACH_EXIT\n',True),
                                      (0,b'BL616CL_PREPARED_ATTACH_ENTER\n',False),
                                      (1,b'BL616CL_PREPARED_ATTACH_NATIVE_TIMEOUT\n',False)]:
            helper=PreparedBlAttach.__new__(PreparedBlAttach)
            helper.process=Mock(returncode=code)
            helper.process.communicate.return_value=(output,b'')
            if success:helper.attach()
            else:
                with self.assertRaises(RuntimeError):helper.attach()
            helper.process.communicate.assert_called_once_with(input=b'ATTACH\n',timeout=20)

    def test_prepared_process_timeout_cleans_up(self):
        helper=PreparedBlAttach.__new__(PreparedBlAttach)
        helper.process=Mock()
        helper.process.poll.return_value=None
        helper.process.communicate.side_effect=subprocess.TimeoutExpired('prepared',20)
        with self.assertRaises(subprocess.TimeoutExpired):helper.attach()
        helper.process.terminate.assert_called_once()
        helper.process.wait.assert_called_once_with(timeout=5)

    def test_prepared_attach_only_for_shared_verified_target(self):
        for identity, state in [('2341:1002','Shared'),('2341:1002','Attached'),
                                ('2341:1002','Not shared'),('ffff:ffff','Shared')]:
            helper=Mock()
            with patch.object(mapping,'usbip_listing',return_value=f'4-1 {identity} Dedicated {state}\n'), \
                 patch.object(mapping,'resolve_paired_bl616cl_port',return_value='/dev/ttyACM7'), \
                 patch.object(mapping.subprocess,'run') as run:
                if identity=='ffff:ffff' or state=='Not shared':
                    with self.assertRaises(RuntimeError):mapping.ensure_bl_usb_mapping(helper)
                else:self.assertEqual(mapping.ensure_bl_usb_mapping(helper),'/dev/ttyACM7')
                self.assertEqual(helper.attach.call_count,int(identity=='2341:1002' and state=='Shared'))
                run.assert_not_called() # no new WSL process during association

    def test_prepared_native_failure_cannot_count_as_reopen(self):
        helper=Mock();helper.attach.side_effect=RuntimeError('native timeout')
        with patch.object(mapping,'usbip_listing',return_value='4-1 2341:1002 Dedicated Shared\n'), \
             patch.object(mapping,'resolve_paired_bl616cl_port') as resolve:
            with self.assertRaises(RuntimeError):mapping.ensure_bl_usb_mapping(helper)
            resolve.assert_called_once_with('auto')
    def test_live_identity_checked_interface_avoids_windows_query(self):
        with patch.object(mapping,'resolve_paired_bl616cl_port',return_value='/dev/ttyACM7'), \
             patch.object(mapping.os,'open',return_value=99) as opened, \
             patch.object(mapping.os,'close') as closed, \
             patch.object(mapping,'dedicated_bus_id') as windows:
            self.assertEqual(mapping.ensure_bl_usb_mapping(),'/dev/ttyACM7')
            opened.assert_called_once()
            closed.assert_called_once_with(99)
            windows.assert_not_called()

    def test_stale_selection(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);controller=root/'devices/platform/vhci_hcd.0';controller.mkdir(parents=True)
            (controller/'status').write_text('hub port sta spd dev sockfd local_busid\nhs 0000 006 003 00040001 000003 1-1\nhs 0001 006 003 00040003 000004 1-2\nhs 0002 004 000 00000000 000000 0-0\n')
            for bus,serial in [('1-1','BL616CL-PROVISIONAL'),('1-2','TESTESP000001')]:
                p=root/'bus/usb/devices'/bus;p.mkdir(parents=True)
                for name,value in [('idVendor','2341'),('idProduct','1002'),('serial',serial)]: (p/name).write_text(value)
            self.assertEqual(mapping.stale_bl_ports(root),[(controller/'detach',0)])
    def test_attached_and_shared(self):
        for state in ('Attached','Shared'):
            listing=subprocess.CompletedProcess([],0,'4-1 2341:1002 Dedicated '+state+'\n')
            with patch.object(mapping.subprocess,'run',return_value=listing) as run,patch.object(mapping,'stale_bl_ports',return_value=[(Path('/sys/devices/platform/vhci_hcd.0/detach'),0)]),patch.object(mapping,'resolve_paired_bl616cl_port',return_value='/dev/ttyACM7'):
                self.assertEqual(mapping.ensure_bl_usb_mapping(),'/dev/ttyACM7')
                calls=[c.args[0] for c in run.call_args_list]
                self.assertEqual(len(calls),1 if state=='Attached' else 3)
                if state=='Shared':
                    self.assertEqual(calls[1][-1],'echo 0 > /sys/devices/platform/vhci_hcd.0/detach')
                    self.assertEqual(calls[2][:3],['usbipd.exe','wsl','attach'])
    def test_wrong_identity_rejected(self):
        with patch.object(mapping.subprocess,'run',return_value=subprocess.CompletedProcess([],0,'4-1 ffff:ffff BootROM Shared\n')),patch.object(mapping,'stale_bl_ports') as stale:
            with self.assertRaises(RuntimeError):mapping.ensure_bl_usb_mapping()
            stale.assert_not_called()
    def test_enumeration_timeout_has_one_read_only_retry(self):
        listing=subprocess.CompletedProcess([],0,'4-1 2341:1002 Dedicated Attached\n')
        for recovered in (False,True):
            results=[subprocess.TimeoutExpired('list',10),listing if recovered else
                     subprocess.TimeoutExpired('list',10)]
            with patch.object(mapping.subprocess,'run',side_effect=results) as run, \
                 patch.object(mapping,'resolve_paired_bl616cl_port',return_value='/dev/ttyACM7'):
                if recovered:self.assertEqual(mapping.ensure_bl_usb_mapping(),'/dev/ttyACM7')
                else:
                    with self.assertRaises(subprocess.TimeoutExpired):mapping.ensure_bl_usb_mapping()
                self.assertEqual([c.args[0] for c in run.call_args_list],[['usbipd.exe','list']]*2)
    def test_attach_timeout_native_fallback(self):
        for final_state in ('Shared', 'Attached', 'wrong'):
            shared=subprocess.CompletedProcess([],0,'4-1 2341:1002 Dedicated Shared\n')
            final=subprocess.CompletedProcess([],0,
                '4-1 ffff:ffff Other Shared\n' if final_state=='wrong' else
                '4-1 2341:1002 Dedicated '+final_state+'\n')
            results=[shared,subprocess.TimeoutExpired('attach',10),final]
            if final_state=='Shared':
                results += [subprocess.CompletedProcess([],0,'default via 172.30.48.1 dev eth0\n'),
                            subprocess.CompletedProcess([],0,'')]
            with patch.object(mapping.subprocess,'run',side_effect=results) as run, \
                 patch.object(mapping,'stale_bl_ports',return_value=[]), \
                 patch.object(mapping,'resolve_paired_bl616cl_port',return_value='/dev/ttyACM7'):
                if final_state=='wrong':
                    with self.assertRaises(RuntimeError):mapping.ensure_bl_usb_mapping()
                else:
                    self.assertEqual(mapping.ensure_bl_usb_mapping(),'/dev/ttyACM7')
                calls=[c.args[0] for c in run.call_args_list]
                if final_state=='Shared':
                    self.assertEqual(calls[-1],['wsl.exe','-d','Ubuntu-22.04','-u','root','--',
                                               'usbip','attach','-r','172.30.48.1','-b','4-1'])
                else:self.assertEqual(len(calls),3)
if __name__=='__main__':unittest.main()
