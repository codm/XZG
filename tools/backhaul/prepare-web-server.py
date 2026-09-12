from pathlib import Path
import os
import runpy

Import('env')
project = Path(env['PROJECT_DIR'])
sdk = env.PioPlatform().get_package_dir('framework-arduinoespressif32')
runpy.run_path(str(project / 'tools/backhaul/patch-web-server.py'))['patch'](sdk)
# Native regression tests must inspect the framework actually used by this build,
# including PlatformIO's suffixed package names when several SDKs are installed.
output = project / '.backhaul-tests'
output.mkdir(exist_ok=True)
(output / 'sdk-path.txt').write_text(os.path.relpath(sdk, project))
