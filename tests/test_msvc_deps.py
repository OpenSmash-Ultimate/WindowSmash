"""tools/msvc_deps.py and its use in tools/cc.py: which files of LLVM's archive and Microsoft's
packages are kept and where they go (the /winsysroot layout), how Visual Studio's channel list and
package manifest are read, and which toolchain cc.py takes. Made-up manifests and packages shaped
like Microsoft's; no downloads, no compiler."""
from pathlib import Path
import io
import json
import os
import shutil
import sys
import tempfile
import unittest
import zipfile
from unittest import mock

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import cc  # noqa: E402
import msvc_deps  # noqa: E402

TOP = 'clang+llvm-23.1.3-x86_64-pc-windows-msvc/'


def payload(name, size=100):
    return {'fileName': name, 'url': 'https://download.example/' + name.replace('\\', '/'), 'sha256': 'a' * 64, 'size': size}


CHANNEL = {
    'info': {'productDisplayVersion': '18.11.0'},
    'channelItems': [
        {'id': 'Microsoft.VisualStudio.Product.Community', 'localizedResources': [
            {'language': 'en-us', 'license': 'https://example/community-license'}]},
        {'id': 'Microsoft.VisualStudio.Product.BuildTools', 'localizedResources': [
            {'language': 'de-de', 'license': 'https://example/lizenz'},
            {'language': 'en-us', 'license': 'https://example/buildtools-license'}]},
        {'id': 'Microsoft.VisualStudio.Manifests.VisualStudio', 'version': '18.11.1',
         'payloads': [payload('VisualStudio.vsman', 18_000_000)]},
    ],
}

SDK_26100 = ['Installers\\' + n for n in msvc_deps.SDK_MSIS] + [
    'Installers\\Windows SDK Desktop Libs arm64-x86_en-us.msi', 'Installers\\' + '1' * 32 + '.cab',
    'Installers\\' + '2' * 32 + '.cab', 'Installers\\' + '3' * 32 + '.cab']
VSMAN = {'packages': [
    # the toolset the unversioned tools package points at, an older one, and a newer preview
    {'id': 'Microsoft.VisualCpp.Tools.HostX64.TargetX64', 'version': '14.51.36260',
     'dependencies': {'Microsoft.VC.14.51.Tools.HostX64.TargetX64.base': '14.51.36260'}},
    {'id': 'Microsoft.VisualStudio.Component.VC.14.50.18.0.x86.x64', 'dependencies': {}},
    {'id': 'Microsoft.VisualStudio.Component.VC.14.51.x86.x64', 'dependencies': {}},
    {'id': 'Microsoft.VisualStudio.Component.VC.14.60.x86.x64', 'dependencies': {}},
    {'id': 'Microsoft.VC.14.51.CRT.Headers.base', 'payloads': [payload('Microsoft.VC.14.51.CRT.Headers.base.vsix', 2_000_000)]},
    {'id': 'Microsoft.VC.14.51.CRT.x64.Desktop.base', 'payloads': [payload('Microsoft.VC.14.51.CRT.x64.Desktop.base.vsix', 67_000_000)]},
    {'id': 'Microsoft.VC.14.51.CRT.x64.Store.base', 'payloads': [payload('Microsoft.VC.14.51.CRT.x64.Store.base.vsix', 30_000_000)]},
    {'id': 'Microsoft.VC.14.51.CRT.Headers.Resources.base', 'language': 'de-DE', 'payloads': [payload('de.vsix')]},
    {'id': 'Microsoft.VC.14.51.CRT.Headers.Resources.base', 'language': 'en-US', 'payloads': [payload('en.vsix')]},
    # the workload recommends 26100; 28000 is newer but optional
    {'id': 'Microsoft.VisualStudio.Workload.VCTools', 'dependencies': {
        'Microsoft.VisualStudio.Component.VC.Tools.x86.x64': {'version': '', 'type': 'Recommended'},
        'Microsoft.VisualStudio.Component.Windows11SDK.26100': {'version': '', 'type': 'Recommended'},
        'Microsoft.VisualStudio.Component.Windows11SDK.28000': {'version': '', 'type': 'Optional'}}},
    {'id': 'Microsoft.VisualStudio.Component.Windows11SDK.26100', 'dependencies': {'Win11SDK_10.0.26100': ''}},
    {'id': 'Microsoft.VisualStudio.Component.Windows11SDK.28000', 'dependencies': {'Win11SDK_10.0.28000': ''}},
    {'id': 'Win11SDK_10.0.26100', 'payloads': [payload(n) for n in SDK_26100]},
    # this one's file names use '/' (Microsoft's manifest has both)
    {'id': 'Win11SDK_10.0.28000', 'payloads': [payload(n.replace('\\', '/')) for n in SDK_26100]},
]}


class KeptFilesTests(unittest.TestCase):
    def test_llvm_members(self):
        keep = {
            TOP + 'bin/clang-cl.exe': 'bin/clang-cl.exe',
            TOP + 'bin/lld-link.exe': 'bin/lld-link.exe',
            TOP + 'bin/llvm-lib.exe': 'bin/llvm-lib.exe',
            TOP + 'lib/clang/23/include/immintrin.h': 'lib/clang/23/include/immintrin.h',
            TOP + 'lib/clang/23/include/cuda_wrappers/new': 'lib/clang/23/include/cuda_wrappers/new',
            TOP + 'lib/clang/23/lib/windows/clang_rt.builtins-x86_64.lib': 'lib/clang/23/lib/windows/clang_rt.builtins-x86_64.lib',
        }
        for name, want in keep.items():
            self.assertEqual(msvc_deps.llvm_member(name), want, name)
        for name in (TOP + 'bin/clang.exe', TOP + 'bin/clang++.exe', TOP + 'bin/libclang.dll', TOP + 'bin/lld.exe',
                     TOP + 'lib/clang/23/lib/windows/clang_rt.asan_dynamic-x86_64.dll', TOP + 'lib/clang/23/include',
                     TOP + 'include/llvm/IR/Module.h', TOP.rstrip('/'), 'bin/clang-cl.exe',
                     TOP + '../bin/clang-cl.exe', TOP + 'lib/clang/23/include/../../../../evil.h', 'C:/x/bin/clang-cl.exe'):
            self.assertIsNone(msvc_deps.llvm_member(name), name)

    def test_vc_members_into_one_toolset_folder(self):
        f = '14.51.36254'
        self.assertEqual(msvc_deps.vc_member('Contents/VC/Tools/MSVC/14.51.36244/include/vcruntime.h', f),
                         'VC/Tools/MSVC/14.51.36254/include/vcruntime.h')   # a headers package with another build number
        self.assertEqual(msvc_deps.vc_member('Contents/VC/Tools/MSVC/14.51.36254/include/cliext/list', f),
                         'VC/Tools/MSVC/14.51.36254/include/cliext/list')
        for lib in ('libcmt.lib', 'libvcruntime.lib', 'oldnames.lib', 'LIBCMT.LIB'):
            self.assertEqual(msvc_deps.vc_member(f'Contents/VC/Tools/MSVC/{f}/lib/x64/{lib}', f), f'VC/Tools/MSVC/{f}/lib/x64/{lib}')
        for name in (f'Contents/VC/Tools/MSVC/{f}/lib/x64/libcmtd.lib', f'Contents/VC/Tools/MSVC/{f}/lib/x64/msvcrt.lib',
                     f'Contents/VC/Tools/MSVC/{f}/lib/x64/libcmt.amd64.pdb', f'Contents/VC/Tools/MSVC/{f}/lib/x86/libcmt.lib',
                     f'Contents/VC/Tools/MSVC/{f}/lib/onecore/x64/libcmt.lib', f'Contents/VC/Tools/MSVC/{f}/bin/Hostx64/x64/cl.exe',
                     f'Contents/VC/Tools/MSVC/{f}/crt/src/vcruntime/frame.cpp', 'Contents/VC/Auxiliary/Build/vcvarsall.bat',
                     '[Content_Types].xml', 'manifest.json', f'Contents/VC/Tools/MSVC/{f}/include/../../evil.h'):
            self.assertIsNone(msvc_deps.vc_member(name, f), name)
        self.assertEqual(msvc_deps.vc_folder(['manifest.json', 'Contents/VC/Tools/MSVC/14.51.36254/lib/x64/libcmt.lib']), '14.51.36254')
        self.assertIsNone(msvc_deps.vc_folder(['manifest.json']))

    def test_sdk_members(self):
        v = '10.0.26100.0'
        for rel in (f'Windows Kits/10/Include/{v}/um/windows.h', f'Windows Kits/10/Include/{v}/shared/dxgi.h',
                    f'Windows Kits/10/Include/{v}/ucrt/stdio.h', f'Windows Kits/10/Include/{v}/um/gl/GL.h',
                    f'Windows Kits/10/Lib/{v}/um/x64/kernel32.Lib', f'Windows Kits/10/Lib/{v}/um/x64/d3dcompiler.lib',
                    f'Windows Kits/10/Lib/{v}/ucrt/x64/libucrt.lib', f'Windows Kits/10/Lib/{v}/ucrt/x64/ucrt.lib',
                    f'windows kits/10/include/{v}/um/Windows.h', f'Windows Kits\\10\\Lib\\{v}\\um\\x64\\user32.lib'):
            self.assertTrue(msvc_deps.sdk_member(rel), rel)
        for rel in (f'Windows Kits/10/Include/{v}/winrt/windows.foundation.h', f'Windows Kits/10/Include/{v}/cppwinrt/winrt/base.h',
                    f'Windows Kits/10/Lib/{v}/um/x86/kernel32.lib', f'Windows Kits/10/Lib/{v}/um/arm64/kernel32.lib',
                    f'Windows Kits/10/Lib/{v}/ucrt/x64/libucrtd.lib', f'Windows Kits/10/Lib/{v}/ucrt/x86/libucrt.lib',
                    f'Windows Kits/10/Source/{v}/ucrt/stdio/printf.cpp', f'Windows Kits/10/bin/{v}/x64/rc.exe',
                    'Windows Kits/10/Include/um/windows.h', 'Windows SDK Desktop Libs x64-x86_en-us.msi'):
            self.assertFalse(msvc_deps.sdk_member(rel), rel)


class ManifestTests(unittest.TestCase):
    def test_channel(self):
        info = msvc_deps.channel_info(CHANNEL)
        self.assertEqual(info['license'], 'https://example/buildtools-license')   # the Build Tools', in English
        self.assertEqual(info['version'], '18.11.0')
        self.assertEqual(info['manifest']['fileName'], 'VisualStudio.vsman')
        for broken in ({'channelItems': []}, {'channelItems': CHANNEL['channelItems'][:2]},
                       {'channelItems': [dict(CHANNEL['channelItems'][1], localizedResources=[]), CHANNEL['channelItems'][2]]}):
            with self.assertRaises(msvc_deps.Failed):
                msvc_deps.channel_info(broken)

    def test_packages_by_id_and_language(self):
        packages = msvc_deps.index_packages(VSMAN)
        self.assertIn('microsoft.vc.14.51.crt.headers.base', packages)
        self.assertEqual(msvc_deps.package(packages, 'MICROSOFT.VC.14.51.CRT.Headers.base')['id'], 'Microsoft.VC.14.51.CRT.Headers.base')
        self.assertEqual(msvc_deps.package(packages, 'Microsoft.VC.14.51.CRT.Headers.Resources.base')['language'], 'en-US')
        self.assertIsNone(msvc_deps.package(packages, 'Microsoft.Nothing'))

    def test_toolset(self):
        packages = msvc_deps.index_packages(VSMAN)
        self.assertEqual(msvc_deps.msvc_version(packages), '14.51')   # the default, not the newer 14.60
        files = msvc_deps.msvc_payloads(packages, '14.51')
        self.assertEqual([f['fileName'] for f in files], ['Microsoft.VC.14.51.CRT.Headers.base.vsix', 'Microsoft.VC.14.51.CRT.x64.Desktop.base.vsix',
                                                          'Microsoft.VC.14.51.CRT.x64.Store.base.vsix'])
        self.assertEqual(files[1]['package'], 'Microsoft.VC.14.51.CRT.x64.Desktop.base')
        with self.assertRaises(msvc_deps.Failed):
            msvc_deps.msvc_payloads(packages, '14.60')   # (its CRT packages are not in this manifest)
        without = msvc_deps.index_packages({'packages': VSMAN['packages'][1:]})
        self.assertEqual(msvc_deps.msvc_version(without), '14.60')   # no default named: the newest
        with self.assertRaises(msvc_deps.Failed):
            msvc_deps.msvc_version({})

    def test_sdk(self):
        packages = msvc_deps.index_packages(VSMAN)
        sdk, version = msvc_deps.sdk_package(packages)
        self.assertEqual((sdk['id'], version), ('Win11SDK_10.0.26100', '10.0.26100'))   # the recommended one
        msis = msvc_deps.sdk_payloads(sdk)
        self.assertEqual([os.path.basename(msvc_deps.payload_name(p)) for p in msis], list(msvc_deps.SDK_MSIS))
        no_workload = msvc_deps.index_packages({'packages': [p for p in VSMAN['packages'] if 'Workload' not in p['id']]})
        sdk, version = msvc_deps.sdk_package(no_workload)
        self.assertEqual(version, '10.0.28000')   # the newest
        self.assertEqual(len(msvc_deps.sdk_payloads(sdk)), len(msvc_deps.SDK_MSIS))   # its '/' names found the same way
        with self.assertRaises(msvc_deps.Failed):
            msvc_deps.sdk_payloads(sdk, ['Windows SDK Nothing-x86_en-us.msi'])

    def test_msi_cabs(self):
        sdk, _ = msvc_deps.sdk_package(msvc_deps.index_packages(VSMAN))
        msi = (b'\xd0\xcf\x11\xe0 junk ' + b'2' * 32 + b'.cab' + b'\x00 ' + b'1' * 32 + b'.cab' + b'2' * 32 + b'.cab'
               + b' ' + b'9' * 32 + b'.cab')   # (9...: named, but not a file of the package)
        cabs = msvc_deps.msi_cabs(msi, sdk)
        self.assertEqual([msvc_deps.payload_name(c) for c in cabs], ['Installers\\' + '1' * 32 + '.cab', 'Installers\\' + '2' * 32 + '.cab'])
        self.assertEqual(msvc_deps.msi_cabs(b'nothing here', sdk), [])


class LayoutTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix='msvc-deps-test-')

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def vsix(self, name, entries):
        path = os.path.join(self.tmp, name)
        with zipfile.ZipFile(path, 'w') as z:
            z.writestr('manifest.json', '{}')
            for n in entries:
                z.writestr(n, 'data of ' + n)
        return path

    def test_winsysroot_layout(self):
        headers = self.vsix('h.vsix', ['Contents/VC/Tools/MSVC/14.51.36244/include/vcruntime.h',
                                       'Contents/VC/Tools/MSVC/14.51.36244/include/stdarg.h'])
        libs = self.vsix('l.vsix', ['Contents/VC/Tools/MSVC/14.51.36254/lib/x64/libcmt.lib',
                                    'Contents/VC/Tools/MSVC/14.51.36254/lib/x64/libcmtd.lib',
                                    'Contents/VC/Tools/MSVC/14.51.36254/lib/x64/libvcruntime.lib',
                                    'Contents/VC/Tools/MSVC/14.51.36254/lib/x64/oldnames.lib',
                                    'Contents/VC/Tools/MSVC/14.51.36254/lib/x86/libcmt.lib'])
        root = os.path.join(self.tmp, 'msvc')
        self.assertEqual(msvc_deps.unpack_vsix([headers, libs], root), '14.51.36254')
        # what msiexec /a leaves: the SDK's folders as they lie in the .msi files, and a copy of each .msi
        unpacked = os.path.join(self.tmp, 'unpacked')
        v = '10.0.26100.0'
        sdk_files = [f'Windows Kits/10/Include/{v}/um/windows.h', f'Windows Kits/10/Include/{v}/um/d3d11.h',
                     f'Windows Kits/10/Include/{v}/um/d3dcompiler.h', f'Windows Kits/10/Include/{v}/um/winsock2.h',
                     f'Windows Kits/10/Include/{v}/shared/dxgi.h', f'Windows Kits/10/Include/{v}/ucrt/stdio.h',
                     f'Windows Kits/10/Include/{v}/winrt/roapi.h', f'Windows Kits/10/Lib/{v}/ucrt/x64/libucrt.lib',
                     f'Windows Kits/10/Lib/{v}/ucrt/x64/libucrtd.lib', f'Windows Kits/10/Lib/{v}/um/x86/kernel32.lib',
                     f'Windows Kits/10/Source/{v}/ucrt/stdio/printf.cpp', 'Universal CRT Headers Libraries and Sources-x86_en-us.msi']
        sdk_files += [f'Windows Kits/10/Lib/{v}/um/x64/{n}' for n in ('kernel32.lib', 'uuid.lib', *cc.SYSTEM_LIBS)]
        for rel in sdk_files:
            Path(unpacked, rel).parent.mkdir(parents=True, exist_ok=True)
            Path(unpacked, rel).write_text(rel)
        self.assertEqual(msvc_deps.collect_sdk(unpacked, root), v)
        msvc_deps.check_msvc(root, '14.51.36254', v)
        got = sorted(str(p.relative_to(root)).replace(os.sep, '/') for p in Path(root).rglob('*') if p.is_file())
        want = sorted(['VC/Tools/MSVC/14.51.36254/include/vcruntime.h', 'VC/Tools/MSVC/14.51.36254/include/stdarg.h',
                       'VC/Tools/MSVC/14.51.36254/lib/x64/libcmt.lib', 'VC/Tools/MSVC/14.51.36254/lib/x64/libvcruntime.lib',
                       'VC/Tools/MSVC/14.51.36254/lib/x64/oldnames.lib'] +
                      [r for r in sdk_files if msvc_deps.sdk_member(r)])
        self.assertEqual(got, want)
        self.assertNotIn(f'Windows Kits/10/Lib/{v}/ucrt/x64/libucrtd.lib', got)
        # one library short: the check names it
        os.remove(os.path.join(root, 'Windows Kits', '10', 'Lib', v, 'um', 'x64', 'd3dcompiler.lib'))
        with self.assertRaisesRegex(msvc_deps.Failed, 'd3dcompiler.lib'):
            msvc_deps.check_msvc(root, '14.51.36254', v)

    def test_vsix_without_libraries(self):
        headers = self.vsix('h.vsix', ['Contents/VC/Tools/MSVC/14.51.36244/include/vcruntime.h'])
        with self.assertRaises(msvc_deps.Failed):
            msvc_deps.unpack_vsix([headers], os.path.join(self.tmp, 'msvc'))


class DownloadTests(unittest.TestCase):
    """download(): the SHA-256 decides (Microsoft's manifest lists some .vsix files with a size
    they don't have, while their SHA-256 is right); a bad file is tried three times, then Failed."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix='msvc-deps-test-')
        self.addCleanup(shutil.rmtree, self.tmp, True)

    def fetch(self, body, sha256=None, size=None):
        import hashlib

        class Response(io.BytesIO):
            headers = {}
        dest = os.path.join(self.tmp, 'f.vsix')
        if os.path.exists(dest):
            os.remove(dest)
        seen = []
        with mock.patch('urllib.request.urlopen', side_effect=lambda req, timeout: Response(body)), \
                mock.patch.object(msvc_deps.time, 'sleep'), mock.patch.object(msvc_deps, 'log'):
            got = msvc_deps.download('https://download.example/f.vsix', dest, sha256 and hashlib.sha256(sha256).hexdigest(),
                                     size, progress=seen.append)
        return got, Path(dest).read_bytes(), sum(seen)

    def test_sha_decides_over_a_wrong_size(self):
        self.assertEqual(self.fetch(b'payload', sha256=b'payload', size=12_345), (7, b'payload', 7))
        with self.assertRaisesRegex(msvc_deps.Failed, 'does not match its SHA-256'):
            self.fetch(b'tampered', sha256=b'payload', size=8)

    def test_size_without_a_sha(self):
        self.assertEqual(self.fetch(b'abc', size=3)[:2], (3, b'abc'))
        with self.assertRaisesRegex(msvc_deps.Failed, '2 bytes downloaded, 3 expected'):
            self.fetch(b'ab', size=3)

    def test_a_file_there_already_is_kept(self):
        import hashlib
        dest = os.path.join(self.tmp, 'f.vsix')
        Path(dest).write_bytes(b'payload')
        seen = []
        with mock.patch('urllib.request.urlopen', side_effect=AssertionError('downloaded')):
            self.assertEqual(msvc_deps.download('https://x/f.vsix', dest, hashlib.sha256(b'payload').hexdigest(), progress=seen.append), 0)
        self.assertEqual(seen, [7])


class InstallFlowTests(unittest.TestCase):
    """install(): Microsoft's license is asked first; no yes, nothing downloaded (exit status 2)."""
    CHANNEL = {'license': 'https://example/buildtools-license', 'version': '18.11.0', 'manifest': {}}
    LLVM = {'version': '23.1.3', 'disk_bytes': 226_000_000, 'download_bytes': 276_000_000}
    MSVC = {'msvc': '14.51.36254', 'sdk': '10.0.26100.0', 'disk_bytes': 320_000_000, 'download_bytes': 300_000_000}

    def run_install(self, answer, accept=False, records=(None, None), agreed=None):
        calls = []
        with mock.patch.object(msvc_deps, 'read_record', side_effect=lambda folder: records[folder == msvc_deps.MSVC_DIR]), \
                mock.patch.object(msvc_deps, 'read_channel', return_value=self.CHANNEL), \
                mock.patch.object(msvc_deps, 'download', side_effect=AssertionError('downloaded')), \
                mock.patch.object(msvc_deps, 'install_llvm', side_effect=lambda keep: calls.append('llvm') or self.LLVM), \
                mock.patch.object(msvc_deps, 'install_msvc', side_effect=lambda ch, keep: calls.append(('msvc', ch['license'])) or self.MSVC), \
                mock.patch.object(msvc_deps, 'test_build', side_effect=lambda: calls.append('check')), \
                mock.patch('sys.stdin', io.StringIO(answer)), mock.patch.object(msvc_deps, 'log') as log:
            code = msvc_deps.install(accept, agreed=agreed)
        return code, calls, '\n'.join(str(c.args[0]) if c.args else '' for c in log.call_args_list)

    def test_declined(self):
        for answer in ('no\n', '\n', '', 'ja\n'):
            code, calls, shown = self.run_install(answer)
            self.assertEqual((code, calls), (2, []), answer)
            self.assertIn('https://example/buildtools-license', shown)
            self.assertIn('nothing was downloaded', shown)

    def test_accepted(self):
        for answer in ('yes\n', 'YES\n', 'y\n'):
            code, calls, shown = self.run_install(answer)
            self.assertEqual(code, 0)
            # (the two folders at the same time, in either order; the test build after both)
            self.assertEqual(sorted(calls[:2], key=str), [('msvc', 'https://example/buildtools-license'), 'llvm'])
            self.assertEqual(calls[2:], ['check'])
        code, calls, shown = self.run_install('', accept=True)   # --accept-license: nothing read
        self.assertEqual((code, len(calls)), (0, 3))
        self.assertIn('accepted (--accept-license)', shown)

    def test_asked_before(self):
        # (the build asks at its first step, then downloads with that answer: nothing asked again)
        code, calls, shown = self.run_install('', agreed=(True, self.CHANNEL))
        self.assertEqual((code, len(calls)), (0, 3))
        self.assertNotIn('Type yes', shown)
        self.assertEqual(self.run_install('yes\n', agreed=(False, self.CHANNEL))[:2], (2, []))

    def test_msvc_there_already(self):
        # (nothing of Microsoft's to download: no question; LLVM is set up if it is not there)
        code, calls, shown = self.run_install('', records=(None, self.MSVC))
        self.assertEqual((code, calls), (0, ['llvm', 'check']))
        self.assertNotIn('license', shown.lower())


class ToolchainChoiceTests(unittest.TestCase):
    """cc._find_toolchain: SSBU_CLANG, then deps (both finished), then Visual Studio's."""
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix='msvc-deps-test-')
        self.llvm, self.msvc = os.path.join(self.tmp, 'llvm'), os.path.join(self.tmp, 'msvc')
        os.makedirs(os.path.join(self.llvm, 'bin'))
        os.makedirs(self.msvc)
        Path(self.llvm, 'bin', 'clang-cl.exe').write_bytes(b'')
        self.no_vs = os.path.join(self.tmp, 'no program files')
        os.makedirs(self.no_vs)

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def choose(self, env=None):
        env = {'ProgramFiles': self.no_vs, 'ProgramFiles(x86)': self.no_vs, **(env or {})}
        with mock.patch.object(cc, 'DEPS_LLVM', self.llvm), mock.patch.object(cc, 'DEPS_MSVC', self.msvc), \
                mock.patch.dict(os.environ, env):
            if 'SSBU_CLANG' not in env:
                os.environ.pop('SSBU_CLANG', None)
            return cc._find_toolchain()

    def test_deps_only_when_both_are_finished(self):
        self.assertEqual(self.choose()[:2], ('clang-cl', None))   # nothing: whatever is on the PATH
        Path(self.llvm, 'windowsmash.json').write_text(json.dumps({'version': '23.1.3'}))
        self.assertEqual(self.choose()[:2], ('clang-cl', None))   # deps\msvc not finished
        Path(self.msvc, 'windowsmash.json').write_text(json.dumps({'msvc': '14.51.36254', 'sdk': '10.0.26100.0'}))
        clang, root, what = self.choose()
        self.assertEqual((clang, root), (os.path.join(self.llvm, 'bin', 'clang-cl.exe'), self.msvc))
        self.assertIn('LLVM 23.1.3', what)
        self.assertIn('MSVC 14.51.36254', what)
        self.assertEqual(self.choose({'SSBU_CLANG': 'C:\\mine\\clang-cl.exe'}), ('C:\\mine\\clang-cl.exe', None, 'SSBU_CLANG'))

    def test_visual_studio_without_deps(self):
        vs = os.path.join(self.no_vs, 'Microsoft Visual Studio', '18', 'BuildTools', 'VC', 'Tools', 'Llvm', 'x64', 'bin')
        os.makedirs(vs)
        Path(vs, 'clang-cl.exe').write_bytes(b'')
        clang, root, what = self.choose()
        self.assertEqual((clang, root), (os.path.join(vs, 'clang-cl.exe'), None))
        self.assertIn('Visual Studio', what)
        Path(self.llvm, 'windowsmash.json').write_text('{}')
        Path(self.msvc, 'windowsmash.json').write_text('{}')
        self.assertEqual(self.choose()[1], self.msvc)   # deps first, Visual Studio or not

    def test_flags_follow_the_system_root(self):
        # (the module's own choice on this PC: /winsysroot exactly when deps\msvc is used)
        self.assertEqual('/winsysroot' in cc.BASE_CFLAGS, cc.WINSYSROOT is not None)
        self.assertEqual('/winsysroot' in cc.LINK_FLAGS, cc.WINSYSROOT is not None)   # (clang-cl's, for the link: test_link_command)
        self.assertEqual(cc.LINK_OPTIONS, ['/winsysroot:' + cc.WINSYSROOT] if cc.WINSYSROOT else [])
        self.assertTrue(set(cc.BASE_CFLAGS) <= set(cc.CFLAGS))


if __name__ == '__main__':
    unittest.main()
