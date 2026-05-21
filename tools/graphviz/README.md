# Graphviz (optional, portable bundle)

The `myir_show` command uses Graphviz's `dot` program to turn the
generated `.dot` file into a `.png` / `.pdf` / `.svg` image.

You have three ways to provide `dot`; `myir_show` tries them in this
order:

1. **Portable bundle in this folder (recommended for one-click demos).**
   Drop a Graphviz distribution so that this path exists:

   ```
   <repo root>/tools/graphviz/bin/dot.exe
   ```

   i.e. the final layout should look like:

   ```
   tools/
     graphviz/
       bin/
         dot.exe
         gvc.dll
         cgraph.dll
         ...       (all the other DLLs from the official zip)
       etc/...
       README.md   (this file)
   ```

   `myir_show` starts at the running executable's directory and walks up
   to three parent levels looking for `tools/graphviz/bin/dot(.exe)`.
   That means dropping the bundle in the repo root works regardless of
   whether Visual Studio builds to `MyVSYosys\x64\Debug\` or elsewhere,
   as long as the exe is under the repo.

   If you prefer to keep Graphviz beside the built executable (so it
   follows the .exe when you copy it elsewhere), the same layout under
   `<exe_dir>/tools/graphviz/bin/` is also accepted.

2. **`MYIR_DOT` environment variable.**  Set it to the full path of
   `dot.exe`, e.g. in PowerShell:

   ```
   $env:MYIR_DOT = "D:\tools\Graphviz\bin\dot.exe"
   ```

3. **System `PATH`.**  Install Graphviz system-wide and tick
   "Add Graphviz to the system PATH" in the installer, or add the
   `bin` folder to `PATH` manually.

## Where to download

* Official project page: <https://graphviz.org/download/>
* Windows portable / ZIP builds are typically available under the
  same download page (look for `graphviz-*-win32.zip` or
  `graphviz-*-win64.zip`).

## Why we do NOT commit the binaries

Graphviz is distributed under the Eclipse Public License (EPL) and
the Windows build is ~30 MB.  Keeping it out of git avoids both the
repository bloat and the obligation to redistribute license texts and
source alongside every clone.  Downloading it once into this folder
is the expected workflow for a full "offline" demo.
