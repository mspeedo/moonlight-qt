# Official upstream Moonlight tests using the shared OTA channel

`master` tracks official `moonlight-stream/moonlight-qt:master` source.
The fork adds only Flatpak packaging, workflows and optional priority helper
source files (outside the Moonlight application).

## Normal build (never publishes OTA)

Pushing to `master` automatically runs **Build - Official Moonlight Flatpak**
and produces a GitHub Actions installable artifact. The R2 preparation and
publication steps are **disabled** on this normal push-triggered run.

## Explicit OTA switch to official Moonlight

1. Open **Actions → Build - Official Moonlight Flatpak → Run workflow**.
2. Select the `master` branch and explicitly enable the boolean
   **publish_r2** option. It defaults to `false`.
3. Confirm the signed build, artifact and R2 publish steps complete.
4. This intentionally publishes official Moonlight code to the SAME R2
   repository as the custom `development` release:
   `https://pub-3a75f66f021b46f3a095b41d1a3c553b.r2.dev/flatpak`.
   It uses the same Flatpak app ID, OSTree branch and signing GPG key.
5. An existing Moonlight development installation will detect the official
   update using its in-app updater, provided Flatpak permits the upgrade.

**IMPORTANT:** Official Moonlight does not include the fork's Updates page.
To switch back, first publish a later `development` build to the SAME R2
repository, then run `flatpak update --user com.moonlight_stream.Moonlight`
from SteamOS Desktop Mode (or `--system` for a system installation).
The weekly development maintenance schedule may also publish development and
supersede the manual master test at any time.

Normal builds and publishing use the SAME source and manifest; publishing is
gated by GitHub Actions event type, branch, manual `publish_r2` input, plus
an additional guard in the publisher script. No manual authorization means
no signed R2 upload. Only the manually approved master workflow can invoke
the R2 prepare/publish steps in this branch.
