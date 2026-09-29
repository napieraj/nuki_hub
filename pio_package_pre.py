Import("env")
import re, shutil, os
from datetime import datetime, timezone

def recursive_purge(dir, pattern):
    if os.path.isdir(dir):
        for f in os.listdir(dir):
            if os.path.isdir(os.path.join(dir, f)):
                recursive_purge(os.path.join(dir, f), pattern)
            elif re.search(pattern, os.path.join(dir, f)):
                os.remove(os.path.join(dir, f))

os.system("python resources/bin2array/bin2array.py icon/favicon-32x32.png -O src/webServerConstants/favicon-32x32.h -l 16")  
os.system("python resources/bin2array/bin2array.py resources/style.css -O src/webServerConstants/style.h -l 16")
os.system("python resources/bin2array/bin2array.py resources/AsyncWebSerial/frontend/index.html -O src/webServerConstants/webSerial.h -l 16")

build_date = datetime.now(timezone.utc).strftime("%Y-%m-%d")

if "NUKI_HUB_WAVESHARE_8DI8RO" in str(env.GetProjectOption("build_flags", "")):
    # Fork build: write the date to a generated, git-ignored header that
    # Config.h includes, so a build never leaves src/Config.h modified.
    generated = "src/webServerConstants/buildDate.h"
    date_content = "#pragma once\n#define NUKI_HUB_DATE \"" + build_date + "\"\n"
    old_content = ""
    if os.path.exists(generated):
        with open(generated, 'r') as readfile:
            old_content = readfile.read()
    if old_content != date_content:
        os.makedirs(os.path.dirname(generated), exist_ok=True)
        with open(generated, 'w') as writefile:
            writefile.write(date_content)
else:
    regex = r"\#define NUKI_HUB_DATE \"(.*)\""
    content_new = ""
    file_content = ""

    with open ('src/Config.h', 'r' ) as readfile:
        file_content = readfile.read()
        content_new = re.sub(regex, "#define NUKI_HUB_DATE \"" + build_date + "\"", file_content, flags = re.M)

    if content_new != file_content:
        with open('src/Config.h', 'w') as writefile:
            writefile.write(content_new)

recursive_purge("managed_components", ".component_hash")

board = env.get('BOARD_MCU')

if os.path.exists("sdkconfig." + board):
  f1 = 0;
  f2 = 0;
  f3 = 0;
  f4 = os.path.getmtime("sdkconfig." + board)
  
  if os.path.exists("sdkconfig.defaults." + board):
    f1 = os.path.getmtime("sdkconfig.defaults." + board)
    
  if os.path.exists("sdkconfig.release.defaults"):
    f2 = os.path.getmtime("sdkconfig.release.defaults")
  
  if os.path.exists("sdkconfig.defaults"):
    f3 = os.path.getmtime("sdkconfig.defaults")

  if(f1 > f4 or f2 > f4 or f3 > f4):
    os.remove("sdkconfig." + board)
  
if os.path.exists("sdkconfig." + board + "_dbg"):
  f1 = 0;
  f2 = 0;
  f3 = 0;
  f4 = os.path.getmtime("sdkconfig." + board + "_dbg")
  
  if os.path.exists("sdkconfig.defaults." + board):
    f1 = os.path.getmtime("sdkconfig.defaults." + board)
    
  if os.path.exists("sdkconfig.debug.defaults"):
    f2 = os.path.getmtime("sdkconfig.debug.defaults")
  
  if os.path.exists("sdkconfig.defaults"):
    f3 = os.path.getmtime("sdkconfig.defaults")

  if(f1 > f4 or f2 > f4 or f3 > f4):
    os.remove("sdkconfig." + board + "_dbg")