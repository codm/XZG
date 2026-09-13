"""Build private TLS profiles from generated inputs in PlatformIO's build directory."""
import importlib.util
from pathlib import Path

Import('env', 'pio_lib_builder')
root = Path(pio_lib_builder.path)
spec = importlib.util.spec_from_file_location('czc_tls_generator', root.parents[1] / 'tools/backhaul/prepare-peer-tls.py')
generator = importlib.util.module_from_spec(spec)
spec.loader.exec_module(generator)

def define_name(item):
    return item[0] if isinstance(item, (tuple, list)) else item

debug = any(define_name(item) == 'DEBUG' for item in env.get('CPPDEFINES', []))
work = Path(env.subst('$BUILD_DIR')) / 'czc-peer-tls'
sources = generator.prepare(work / 'generated', debug)
# This environment owns both compiler include paths and dependency scanning.
# Only the resulting archive is exported to other PlatformIO libraries.
private = env.Clone()
private.Replace(CPPDEFINES=[item for item in private.get('CPPDEFINES', [])
                           if define_name(item) != 'MBEDTLS_CONFIG_FILE'])
private.Append(CPPDEFINES=[('MBEDTLS_CONFIG_FILE', '\\"CzcTlsConfig.h\\"')])
private.Prepend(CPPPATH=[str(root / 'src'), str(sources), str(root / 'mbedtls'),
                        str(root / 'mbedtls/include'), str(root / 'mbedtls/library')])
private.Append(CCFLAGS=['-flto'])
nodes = private.CollectBuildFiles(str(work / 'adapter'), str(root / 'src'), ['+<*.c>'])
nodes += private.CollectBuildFiles(str(work / 'profiles'), str(sources), ['+<*>'])
archive = private.BuildLibrary(str(work / 'CzcPeerTls'), str(sources), nodes=nodes)
env.Append(LIBS=[archive], LINKFLAGS=['-flto'])
# The adapter sources are compiled into the private archive above.
env.Replace(SRC_FILTER=['-<*>'])
env.Clean(archive, str(sources))
