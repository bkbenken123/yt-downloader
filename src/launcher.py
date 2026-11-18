import os
import subprocess
import sys


def main():
    root = getattr(sys, "_MEIPASS", os.path.dirname(os.path.abspath(__file__)))
    app_directory = os.path.join(root, "downloader")
    app = os.path.join(app_directory, "downloader.exe")
    if not os.path.isfile(app):
        raise SystemExit("Bundled Downloader application is missing")

    os.environ["DOWNLOADER_BUNDLED_ROOT"] = root
    config_root = os.path.join(
        os.environ.get("LOCALAPPDATA", os.path.expanduser("~")),
        "Downloader",
    )
    os.makedirs(config_root, exist_ok=True)
    os.environ["DOWNLOADER_CONFIG_ROOT"] = config_root
    os.environ["QT_PLUGIN_PATH"] = os.path.join(app_directory, "plugins")
    subprocess.run([app] + sys.argv[1:], cwd=os.path.expanduser("~"), check=False)


if __name__ == "__main__":
    main()
