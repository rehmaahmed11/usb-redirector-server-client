# Optional offline backend installers

Drop the **unmodified** official installers here to make the setup fully offline:

* `usbipd-win*.msi` — from https://github.com/dorssel/usbipd-win/releases (GPL-3.0)
* `usbip-win2*.exe` — from https://github.com/vadimgrn/usbip-win2/releases (BSD-2-Clause)

The installer detects these files, offers to run them after installation, and deletes them from the
temporary folder afterwards. They are intentionally **not** stored in this repository (size and license
hygiene): each vendor keeps its own licensing terms, and GPL-3.0 requires you to be able to provide the
corresponding source when you redistribute the binary. Point users at the official releases instead, or
mirror them yourself with the sources.
