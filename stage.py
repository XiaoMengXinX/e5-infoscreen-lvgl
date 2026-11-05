"""Stage the shared info screen with the LVGL program as its interface."""
from pathlib import Path
import runpy

HERE = Path(__file__).resolve().parent


def stage(root):
    root = Path(root)
    build = runpy.run_path(str(HERE / 'build.py'))
    runpy.run_path(str(build['E5_LINUX'] / 'devices/common/infoscreen/stage.py'))['stage'](root)
    binary = build['build']()
    for source, name in [(binary, 'usr/libexec/e5-infoscreen/ui'),
                         (HERE / 'session', 'usr/libexec/e5-infoscreen/session')]:
        dest = root / name
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(Path(source).read_bytes())
        dest.chmod(0o755)


if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root')
    stage(parser.parse_args().root)
