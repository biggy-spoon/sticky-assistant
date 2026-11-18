# Seeed Sticky Playground entry

`sticky-assistant/` is this project's entry for the
[reTerminal Sticky Playground](https://www.seeedstudio.com/sticky/playground/firmware/),
laid out the way the
[Playground registry](https://github.com/Seeed-Projects/reterminal-sticky-playground-registry)
expects (`firmwares/sticky-assistant/` there). It's a firmware-only entry:
the binaries come from this project's release workflow, not a build in the
registry.

To submit a version:

1. Tag it: `git tag v1.0.0 && git push origin v1.0.0`. The release workflow
   builds a draft GitHub release; check it and publish it.
2. Unzip the release's `sticky-assistant-1.0.0.zip` into
   `sticky-assistant/firmware/`, giving `firmware/1.0.0/manifest.json` and the
   three `.bin` files. Update `flash.versions` in `firmware.json` if the
   version changed.
3. Add `sticky-assistant/assets/preview.jpg`: a real photo of a Sticky running
   it (the registry requires a real photo, not a mock-up).
4. Flash those exact files to a Sticky after an erase, go through the checklist
   in `sticky-assistant/README.md` and fill in the result.
5. In a fork of the registry, copy `sticky-assistant/` to
   `firmwares/sticky-assistant/`, run `npm test && npm run validate`,
   and open a pull request with the hardware test result in its description.

The `.bin` files and the photo aren't kept here; they belong in the registry.
