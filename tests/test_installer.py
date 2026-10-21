"""The installer's decisions (tools/install_windowsmash.ps1): its pure functions, run in Windows
PowerShell with made-up inputs. No downloads, no installs, no game files."""
from pathlib import Path
import json
import os
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
PS = Path(os.environ.get('SystemRoot', r'C:\Windows')) / 'System32/WindowsPowerShell/v1.0/powershell.exe'
SCRIPT = ROOT / 'tools/install_windowsmash.ps1'


def ps_text(script):
    """What PowerShell prints running `script` with the installer's functions loaded (its host output)."""
    command = f". '{SCRIPT}'; {script}"
    r = subprocess.run([str(PS), '-NoProfile', '-ExecutionPolicy', 'Bypass', '-Command', command],
                       capture_output=True, text=True)
    return r.stdout + r.stderr


def ps(expression):
    """The JSON of a PowerShell expression evaluated with the installer's functions loaded."""
    command = f". '{SCRIPT}'; {expression} | ConvertTo-Json -Compress -Depth 6"
    r = subprocess.run([str(PS), '-NoProfile', '-ExecutionPolicy', 'Bypass', '-Command', command],
                       capture_output=True, text=True)
    if r.returncode or not r.stdout.strip():
        raise AssertionError(r.stdout + r.stderr)
    return json.loads(r.stdout)


SDL_RELEASES = json.dumps([
    {"tag_name": "release-3.5.0", "prerelease": True, "draft": False,
     "assets": [{"name": "SDL3-devel-3.5.0-VC.zip", "browser_download_url": "https://x/pre.zip", "digest": "sha256:" + "a" * 64}]},
    {"tag_name": "release-3.4.20", "prerelease": False, "draft": False,
     "assets": [{"name": "SDL3-devel-3.4.20-mingw.zip", "browser_download_url": "https://x/mingw.zip"}]},
    {"tag_name": "release-3.4.16", "prerelease": False, "draft": False,
     "assets": [{"name": "SDL3-devel-3.4.16-VC.zip", "browser_download_url": "https://x/16.zip", "digest": "sha256:" + "b" * 64}]},
    {"tag_name": "release-3.4.18", "prerelease": False, "draft": False,
     "assets": [{"name": "SDL3-3.4.18.tar.gz", "browser_download_url": "https://x/src.tar.gz"},
                {"name": "SDL3-devel-3.4.18-VC.zip", "browser_download_url": "https://x/18.zip", "digest": "sha256:" + "C" * 64}]},
    {"tag_name": "release-2.32.0", "prerelease": False, "draft": False,
     "assets": [{"name": "SDL2-devel-2.32.0-VC.zip", "browser_download_url": "https://x/sdl2.zip"}]},
])


@unittest.skipUnless(PS.exists(), 'Windows PowerShell only')
class InstallerDecisionTests(unittest.TestCase):
    def test_python_embed_url_newest_of_the_minor(self):
        listing = 'href="3.14.6/" href="3.9.1/" href="3.14.8/" href="3.15.0/" href="3.14.10/"'
        got = ps(f"@{{ pick = (Get-PythonEmbedUrl '{listing}' '3.14' '3.14.0'); none = (Get-PythonEmbedUrl '' '3.14' '3.14.8') }}")
        self.assertEqual(got['pick']['Version'], '3.14.10')
        self.assertEqual(got['pick']['Url'], 'https://www.python.org/ftp/python/3.14.10/python-3.14.10-embed-amd64.zip')
        self.assertEqual(got['none']['Version'], '3.14.8')
        self.assertTrue(got['none']['Url'].endswith('/3.14.8/python-3.14.8-embed-amd64.zip'))

    def test_sdl3_asset_newest_release_with_a_vc_zip(self):
        releases = SDL_RELEASES.replace("'", "''")
        got = ps(f"$r = (Select-Sdl3Asset ('{releases}' | ConvertFrom-Json)); @{{ v = $r.Version.ToString(); url = $r.Url; sha = $r.Sha256; name = $r.Name }}")
        self.assertEqual(got['v'], '3.4.18')   # not the prerelease 3.5.0, not 3.4.20 without a VC zip
        self.assertEqual(got['url'], 'https://x/18.zip')
        self.assertEqual(got['sha'], 'c' * 64)
        self.assertEqual(got['name'], 'SDL3-devel-3.4.18-VC.zip')

    def test_sdl3_asset_without_digest_or_releases(self):
        releases = json.dumps([{"tag_name": "release-3.4.4", "prerelease": False, "draft": False,
                                "assets": [{"name": "SDL3-devel-3.4.4-VC.zip", "browser_download_url": "https://x/4.zip"}]}])
        got = ps(f"$r = (Select-Sdl3Asset ('{releases}' | ConvertFrom-Json)); @{{ v = $r.Version.ToString(); sha = $r.Sha256; none = (Select-Sdl3Asset @()) }}")
        self.assertEqual(got['v'], '3.4.4')
        self.assertIsNone(got['sha'])
        self.assertIsNone(got['none'])

    def test_visual_studio_arguments(self):
        got = ps("@{ install = (Get-VsBuildToolsArguments 'Microsoft.VisualStudio.BuildTools'); "
                 "bt = (Get-VsModifyArguments 'C:\\VS\\BuildTools' 'Microsoft.VisualStudio.Product.BuildTools'); "
                 "community = (Get-VsModifyArguments 'C:\\VS\\Community' 'Microsoft.VisualStudio.Product.Community') }")
        install = got['install']
        self.assertEqual(install[:3], ['install', '--id', 'Microsoft.VisualStudio.BuildTools'])
        self.assertIn('--accept-package-agreements', install)
        self.assertIn('--accept-source-agreements', install)
        override = install[install.index('--override') + 1]
        for part in ('--passive', '--wait', '--norestart', '--add Microsoft.VisualStudio.Workload.VCTools',
                     '--add Microsoft.VisualStudio.Component.VC.Llvm.Clang', '--includeRecommended'):
            self.assertIn(part, override)
        self.assertEqual(got['bt'][:3], ['modify', '--installPath', 'C:\\VS\\BuildTools'])
        self.assertIn('Microsoft.VisualStudio.Workload.VCTools', got['bt'])
        self.assertIn('Microsoft.VisualStudio.Workload.NativeDesktop', got['community'])
        self.assertIn('Microsoft.VisualStudio.Component.VC.Llvm.Clang', got['community'])
        self.assertIn('--passive', got['community'])
        self.assertNotIn('--wait', got['community'])   # (setup.exe does not take it)
        self.assertIn('--wait', override)              # (the bootstrapper winget runs does)

    def test_visual_studio_modify_command_line(self):
        # setup.exe gets one quoted command line (a list reached it unquoted: the path broke at its
        # spaces, exit code 87), no --wait, which only the bootstrapper takes, and administrator
        # rights from the start (without them it refuses --passive: exit code 5007)
        path = 'C:\\Program Files\\Microsoft Visual Studio\\2022\\Community'
        with tempfile.TemporaryDirectory(prefix='installer-test-') as tmp:
            Path(tmp, 'setup.exe').write_bytes(b'')
            script = (f"$VsInstallerDir = '{tmp}'; $global:n = 0; "
                      "function Install-PortableCompiler { 'failed' }; "   # (the download route first; it failed)
                      "function Test-Compiler { $global:n++; @{ Ok = ($global:n -gt 1); Output = 'clang-cl: ok' } }; "
                      f"function Get-VsInstances {{ @{{ Path = '{path}'; Product = 'Microsoft.VisualStudio.Product.Community' }} }}; "
                      "function Start-Process { param($FilePath, $ArgumentList, $Verb, [switch]$Wait, [switch]$PassThru) "
                      "  $global:seen = $ArgumentList; $global:verb = $Verb; "
                      "  if ($global:code -eq 'declined') { throw 'The operation was canceled by the user.' } "
                      "  [pscustomobject]@{ ExitCode = $global:code } }; ")
            out = ps_text(script + "$global:code = 0; Step-Compiler; 'IS STRING: ' + ($global:seen -is [string]); "
                          "'VERB: ' + $global:verb; 'LINE: ' + $global:seen")
            self.assertIn('IS STRING: True', out)
            self.assertIn('VERB: RunAs', out)
            line = out.split('LINE: ', 1)[1].strip()
            r = subprocess.run(f'"{sys.executable}" -c "import sys, json; print(json.dumps(sys.argv[1:]))" {line}',
                               capture_output=True, text=True)
            argv = json.loads(r.stdout)
            self.assertEqual(argv[:3], ['modify', '--installPath', path])
            self.assertIn('--passive', argv)
            self.assertNotIn('--wait', argv)
            out = ps_text(script + "$global:code = 8006; try { Step-Compiler } catch { 'STOPPED: ' + $_.Exception.Message }")
            self.assertIn('STOPPED: The Visual Studio Installer could not add the C++ tools: Visual Studio is running', out)
            self.assertIn('(code 8006)', out)
            out = ps_text(script + "$global:code = 'declined'; try { Step-Compiler } catch { 'STOPPED: ' + $_.Exception.Message }")
            self.assertIn('STOPPED: The Visual Studio Installer needs administrator rights', out)
            self.assertIn('click Yes when Windows asks', out)

    def test_visual_studio_exit_codes(self):
        got = ps("@{ ok = (Get-VsExitMessage 0); reboot = (Get-VsExitMessage 3010); running = (Get-VsExitMessage 8006); "
                 "bad = (Get-VsExitMessage 87); blocked = (Get-VsExitMessage 5007); other = (Get-VsExitMessage 1603) }")
        self.assertIsNone(got['ok'])
        self.assertIsNone(got['reboot'])
        self.assertIn('close it', got['running'])
        self.assertIn('options', got['bad'])
        self.assertIn('administrator rights', got['blocked'])
        self.assertEqual(got['other'], 'it stopped')

    def test_folder_candidates_and_resolution(self):
        got = ps("$c = Get-FolderCandidates 'C:\\Users\\me\\AppData\\Roaming' 'C:\\Users\\me'; "
                 "$have = @('C:\\Users\\me\\AppData\\Roaming\\Ryujinx\\system', 'C:\\Users\\me\\.switch'); "
                 "@{ keys0 = $c.Keys[0]; firmware0 = $c.Firmware[0]; game0 = $c.Game[0]; "
                 "ryu = (Resolve-FirstExisting $c.Keys { param($p) $have -contains $p }); "
                 "none = (Resolve-FirstExisting $c.Game { param($p) $false }); "
                 "ryugame = ($c.Game -contains 'C:\\Users\\me\\AppData\\Roaming\\Ryujinx\\bis\\user\\Contents\\registered'); "
                 "ryufw = ($c.Firmware -contains 'C:\\Users\\me\\AppData\\Roaming\\Ryujinx\\bis\\system\\Contents\\registered'); "
                 "switch = ($c.Keys -contains 'C:\\Users\\me\\.switch') }")
        self.assertEqual(got['keys0'], 'C:\\Users\\me\\AppData\\Roaming\\yuzu\\keys')
        self.assertEqual(got['firmware0'], 'C:\\Users\\me\\AppData\\Roaming\\yuzu\\nand\\system\\Contents\\registered')
        self.assertEqual(got['game0'], 'C:\\Users\\me\\AppData\\Roaming\\yuzu\\nand\\user\\Contents\\registered')
        self.assertEqual(got['ryu'], 'C:\\Users\\me\\AppData\\Roaming\\Ryujinx\\system')   # the first that exists
        self.assertIsNone(got['none'])
        self.assertTrue(got['ryugame'] and got['ryufw'] and got['switch'])

    def test_avx2_by_processor_name(self):
        cases = {
            'Intel(R) Core(TM) i5-4590 CPU @ 3.30GHz': True,
            'Intel(R) Core(TM) i7-12700K': True,
            'Intel(R) Core(TM) i7-3770 CPU @ 3.40GHz': False,
            'Intel(R) Core(TM) Ultra 7 155H': True,
            'AMD Ryzen 5 3600 6-Core Processor': True,
            'AMD EPYC 7302P': True,
            'Intel(R) Core(TM)2 Duo CPU E8400': False,
            'Intel(R) Celeron(R) N4000 CPU': None,
            'Intel(R) Xeon(R) Gold 6148': None,
        }
        names = '; '.join(f"'{n}' = (Test-Avx2ByName '{n}')" for n in cases)
        got = ps('@{ ' + names + ' }')
        for name, want in cases.items():
            self.assertEqual(got[name], want, name)

    def test_pc_check_without_csharp_compiler(self):
        # a PC whose .NET C# compiler is broken: Add-Type -MemberDefinition stopped step 2 with
        # "Unable to find messages file 'cscui.dll'"; the AVX2 question must not need the compiler
        mocks = ("function Add-Type { throw '(0) : Unable to find messages file ''cscui.dll''' }; "
                 "function Get-PSDrive { $null }; "
                 "function Get-CimInstance { [CmdletBinding()] param([string]$ClassName) switch ($ClassName) { "
                 "'Win32_OperatingSystem' { [pscustomobject]@{ Caption = 'Microsoft Windows 10 Home'; Version = '10.0.19045'; "
                 "BuildNumber = '19045'; OSArchitecture = '64-bit' } } "
                 "'Win32_Processor' { [pscustomobject]@{ Name = 'AMD Ryzen 7 3700X 8-Core Processor' } } "
                 "'Win32_ComputerSystem' { [pscustomobject]@{ TotalPhysicalMemory = 40GB } } } }; ")
        step = "try { Step-CheckPc } catch { 'STOPPED: ' + $_.Exception.Message }"
        got = ps(mocks + "@{ sse2 = (Test-CpuFeature 10); avx2 = (Test-CpuFeature 40) }")
        self.assertIs(got['sse2'], True)              # every x64 processor: Windows answered
        self.assertIn(got['avx2'], (True, False))
        out = ps_text(mocks + step)
        self.assertNotIn('cscui', out)
        self.assertIn('AMD Ryzen 7 3700X 8-Core Processor - has', out)
        out = ps_text(mocks + "function Test-CpuFeature { $null }; " + step)   # Windows cannot be asked: the name
        self.assertIn('AMD Ryzen 7 3700X 8-Core Processor - has AVX2', out)
        self.assertNotIn('STOPPED', out)
        out = ps_text(mocks + "function Test-CpuFeature { $false }; " + step)
        self.assertIn('STOPPED: The processor (AMD Ryzen 7 3700X 8-Core Processor) has no AVX2', out)

    def test_source_version(self):
        sha = '9fdc8fd21b3fde86c3e58a1e634a7e76a87220c7'
        got = ps(f"@{{ zip = (Read-SourceVersion '{sha.upper()} 2026-10-19'); git = (Read-SourceVersion '$Format:%H %cs$'); "
                 f"none = (Read-SourceVersion ''); long = (Read-SourceVersion '{sha}0') }}")
        self.assertEqual(got['zip'], sha)
        self.assertIsNone(got['git'])     # a git checkout: the placeholder
        self.assertIsNone(got['none'])
        self.assertIsNone(got['long'])    # 41 hex digits are not a commit

    def test_source_version_filled_in_by_downloads(self):
        attributes = (ROOT / '.gitattributes').read_text()
        self.assertIn('tools/source_version.txt export-subst', attributes.splitlines())
        self.assertIn('$Format:%H', (ROOT / 'tools/source_version.txt').read_text())

    def test_update_decision(self):
        got = ps("@{ a = (Get-UpdateDecision 'ahead'); i = (Get-UpdateDecision 'identical'); b = (Get-UpdateDecision 'behind'); "
                 "d = (Get-UpdateDecision 'diverged'); x = (Get-UpdateDecision 'something') }")
        self.assertEqual(got, {'a': 'update', 'i': 'current', 'b': 'newer', 'd': 'diverged', 'x': 'unknown'})

    def test_removed_paths(self):
        got = ps("@{ r = (Get-RemovedPaths @('tools/a.py', 'tools/old.py', 'README.md', 'docs/Gone.md', '../evil.txt', 'x/../../y', 'C:/z') "
                 "@('tools/a.py', 'readme.md', 'tools/new.py')); none = (Get-RemovedPaths @('a') @('A')) }")
        self.assertEqual(got['r'], ['tools/old.py', 'docs/Gone.md'])   # case-only renames kept, escapes refused
        self.assertEqual(got['none'], [])

    def test_online_server_merge(self):
        head = '# The online lobby\n# host:port, then an optional lobby code\n'
        old, new = head + 'lobby.example:39420\n', head + 'lobby.example:39421\n'
        cases = {
            'untouched': (old, old, new, new),
            'code added': (head + 'lobby.example:39420 FRIENDS\n', old, new, head + 'lobby.example:39421 FRIENDS\n'),
            'code, CRLF': ((head + 'lobby.example:39420   FRIENDS\n').replace('\n', '\r\n'), old, new, head + 'lobby.example:39421 FRIENDS\n'),
            'own lobby': (head + 'my.server:5000\n', old, new, head + 'my.server:5000\n'),
            'off': (head + 'off\n', old, new, head + 'off\n'),
            'comment only': ('# mine\n' + head + 'lobby.example:39420\n', old, new, new),
            'old unknown': (head + 'lobby.example:39420 FRIENDS\n', None, new, head + 'lobby.example:39420 FRIENDS\n'),
            'no file': (None, old, new, new),
            'same as new': (new, old, new, new),
        }
        def lit(s):
            return '$null' if s is None else "'" + s.replace("'", "''") + "'"
        expr = '@{ ' + '; '.join(f"'{k}' = (Merge-OnlineServer {lit(l)} {lit(o)} {lit(n)})" for k, (l, o, n, _) in cases.items()) + ' }'
        got = ps(expr)
        for k, (_l, _o, _n, want) in cases.items():
            self.assertEqual(got[k].replace('\r\n', '\n'), want, k)

    def test_command_line_round_trip(self):
        args = ['plain', 'with space', 'quote"inside', 'trailing\\', 'back\\"quote', 'C:\\My Dump\\', '']
        lit = ', '.join("'" + a.replace("'", "''") + "'" for a in args)
        line = ps(f"@{{ line = (Join-CommandLine @({lit})) }}")['line']
        r = subprocess.run(f'"{sys.executable}" -c "import sys, json; print(json.dumps(sys.argv[1:]))" {line}',
                           capture_output=True, text=True)
        self.assertEqual(json.loads(r.stdout), args, line)

    def test_restart_arguments(self):
        got = ps("@{ a = (Get-RestartArguments @{ DryRun = [switch]$true; SkipBuild = [switch]$false; Game = 'D:\\My Dump'; Jobs = 4 }); "
                 "b = (Get-RestartArguments @{ NoUpdate = [switch]$true }); c = (Get-RestartArguments @{}) }")
        self.assertEqual(got['a'], ['-DryRun', '-Game', 'D:\\My Dump', '-Jobs', '4', '-NoUpdate'])
        self.assertEqual(got['b'], ['-NoUpdate'])
        self.assertEqual(got['c'], ['-NoUpdate'])

    def test_compiler_step_without_clang(self):
        # (the first run on a PC without the compiler: no Visual Studio, one edition, two editions, no winget;
        # the dry run says the download route comes first, with msvc_deps.py plan's lines, then the Build Tools)
        with tempfile.TemporaryDirectory(prefix='installer-test-') as tmp:
            Path(tmp, 'setup.exe').write_bytes(b'')
            mocks = (f"$DryRun = $true; $VsInstallerDir = '{tmp}'; "
                     "function Get-PythonOutput { param([string[]]$Arguments) $global:planned = ($Arguments -join ' '); "
                     "  @{ Output = ('LLVM 23.1.3 from GitHub: https://x/llvm.tar.zst' + [char]10 + "
                     "'  only after you accept Microsoft''s license terms: https://x/license'); Code = 0 } }; "
                     "function Test-Compiler { @{ Ok = $false; Output = 'ERROR: clang-cl.exe was not found' } }; ")
            winget = "function Get-Command { [pscustomobject]@{ Source = 'C:\\winget.exe' } }; "
            one = "@{ Path = 'C:\\VS\\BuildTools'; Product = 'Microsoft.VisualStudio.Product.BuildTools' }"
            two = one + "; @{ Path = 'C:\\VS\\Community'; Product = 'Microsoft.VisualStudio.Product.Community' }"
            cases = {
                'none': (winget + "function Get-VsInstances { return @() }; ", 'would run: winget install --id Microsoft.VisualStudio.BuildTools'),
                'one': (winget + f"function Get-VsInstances {{ {one} }}; ", 'Visual Studio in C:\\VS\\BuildTools: setup.exe modify'),
                'two': (winget + f"function Get-VsInstances {{ {two} }}; ", 'Visual Studio in C:\\VS\\BuildTools: setup.exe modify'),
                'no winget': ("function Get-Command { $null }; function Get-VsInstances { }; ", 'visual-cpp-build-tools'),
            }
            for name, (setup, want) in cases.items():
                out = ps_text(mocks + setup + "try { Step-Compiler } catch { 'STOPPED: ' + $_.Exception.Message }; 'PLAN: ' + $global:planned")
                self.assertIn(want, out, name)
                self.assertNotIn("property 'Count' cannot be found", out, name)
                self.assertIn('would run tools\\msvc_deps.py install', out, name)
                self.assertIn("only after you accept Microsoft's license terms: https://x/license", out, name)
                self.assertIn('msvc_deps.py plan', out, name)
                self.assertLess(out.index('msvc_deps.py install'), out.index(want), name)   # that route first
                if name != 'no winget':
                    self.assertNotIn('STOPPED', out, name)
            out = ps_text(mocks + cases['none'][0] + "$BuildTools = $true; Step-Compiler")   # -BuildTools: straight to them
            self.assertNotIn('msvc_deps', out)
            self.assertIn('would run: winget install --id Microsoft.VisualStudio.BuildTools', out)

    def test_compiler_step_download_route(self):
        # the download route's outcomes: it works; declined (stops: the Build Tools are under the same
        # license); it fails, or works but the check after it does not (removed): the Build Tools next
        with tempfile.TemporaryDirectory(prefix='installer-test-') as tmp:
            Path(tmp, 'setup.exe').write_bytes(b'')
            mocks = (f"$VsInstallerDir = '{tmp}'; $global:n = 0; $global:removed = $false; $global:vs = $false; "
                     "function Test-Compiler { $global:n++; if ($global:n -le $global:fails) { @{ Ok = $false; Output = 'ERROR: no compiler' } } "
                     "  else { @{ Ok = $true; Output = ('  clang-cl: C:\\x\\clang-cl.exe' + [char]10 + '  toolchain: deps\\llvm (LLVM 23.1.3) with deps\\msvc') } } }; "
                     "function Install-PortableCompiler { $global:how }; "
                     "function Remove-PortableCompiler { $global:removed = $true }; "
                     "function Get-VsInstances { @{ Path = 'C:\\VS\\BuildTools'; Product = 'Microsoft.VisualStudio.Product.BuildTools' } }; "
                     "function Start-Process { param($FilePath, $ArgumentList, $Verb, [switch]$Wait, [switch]$PassThru) "
                     "  $global:vs = $true; [pscustomobject]@{ ExitCode = 0 } }; ")
            report = "; 'REMOVED: ' + $global:removed; 'VS: ' + $global:vs; 'CHECKS: ' + $global:n"
            out = ps_text(mocks + "$global:how = 'done'; $global:fails = 1; Step-Compiler" + report)
            self.assertIn('toolchain: deps\\llvm (LLVM 23.1.3)', out)
            self.assertIn('VS: False', out)
            self.assertIn('CHECKS: 2', out)
            out = ps_text(mocks + "$global:how = 'declined'; $global:fails = 1; try { Step-Compiler } catch { 'STOPPED: ' + $_.Exception.Message }" + report)
            self.assertIn("STOPPED: Microsoft's license terms were not accepted", out)
            self.assertIn('VS: False', out)
            out = ps_text(mocks + "$global:how = 'failed'; $global:fails = 1; Step-Compiler" + report)
            self.assertIn('installing the Visual Studio Build Tools instead', out)
            self.assertIn('VS: True', out)
            self.assertIn('REMOVED: False', out)
            out = ps_text(mocks + "$global:how = 'done'; $global:fails = 2; Step-Compiler" + report)
            self.assertIn('the downloaded compiler did not pass the check', out)
            self.assertIn('REMOVED: True', out)
            self.assertIn('VS: True', out)
            self.assertIn('CHECKS: 3', out)

    def test_zstd_check_before_python_314(self):
        # Python 3.13 and older have no compression package: asking for compression.zstd raised
        # there, so a Python with zstandard installed was taken for one without it
        check = ps("@{ code = $ZstdCheck }")['code']
        self.assertNotIn('"', check)
        older = ("import sys, types, importlib.machinery; sys.modules['compression'] = None; "
                 "sys.version_info = (3, 13, 7, 'final', 0); ")
        fake_zstandard = ("m = types.ModuleType('zstandard'); m.__spec__ = importlib.machinery.ModuleSpec('zstandard', None); "
                          "sys.modules['zstandard'] = m; ")
        no_zstandard = "import sys; sys.modules['zstandard'] = None; "
        def run(prefix):
            return subprocess.run([sys.executable, '-c', prefix + f'exec({check!r})'], capture_output=True, text=True)
        r = run(older + fake_zstandard)
        self.assertEqual(r.returncode, 0, r.stderr)                 # 3.13 with zstandard: there
        r = run(older + no_zstandard)
        self.assertEqual((r.returncode, r.stderr), (1, ''))         # 3.13 without: a plain no, no error
        if sys.version_info >= (3, 14):
            r = run(no_zstandard)
            self.assertEqual(r.returncode, 0, r.stderr)             # 3.14: compression.zstd

    def test_small_decisions(self):
        got = ps("@{ fw = (Get-FirewallRuleCommand 'C:\\WindowSmash\\build\\ssbu.exe' 'WindowSmash (ssbu.exe)'); "
                 "v1 = (Test-PythonVersionText 'Python 3.14.6'); v2 = (Test-PythonVersionText '3.10.0'); "
                 "v3 = (Test-PythonVersionText '3.9.13'); v4 = (Test-PythonVersionText 'Python was not found'); "
                 "v5 = (Test-PythonVersionText '2.7.18'); "
                 "d1 = (Get-DiskNeedGb 60 25); d2 = (Get-DiskNeedGb 60 70) }")
        self.assertIn('New-NetFirewallRule', got['fw'])
        self.assertIn("-Program 'C:\\WindowSmash\\build\\ssbu.exe'", got['fw'])
        self.assertIn("-DisplayName 'WindowSmash (ssbu.exe)'", got['fw'])
        self.assertIn('-Direction Inbound', got['fw'])
        self.assertEqual([got['v1'], got['v2'], got['v3'], got['v4'], got['v5']], [True, True, False, False, False])
        self.assertEqual(got['d1'], 35)
        self.assertEqual(got['d2'], 0)


if __name__ == '__main__':
    unittest.main()
