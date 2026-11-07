#!/usr/bin/env bash
# Publish the fork's Flatpak OSTree repository to Cloudflare R2.
set -euo pipefail

die() { echo "::error::$*" >&2; exit 1; }

: "${GITHUB_OUTPUT:=/dev/null}"

# Fail promptly on API errors instead of retrying thousands of objects.
# The explicit timeout limits the entire copy; a subsequent build can
# continue any already-uploaded, content-addressed OSTree objects.
rclone_checked() {
  timeout --signal=TERM 480s rclone --retries 1 --low-level-retries 1 \
    --contimeout 12s --timeout 35s "$@"
}

# The GitHub job can override GITHUB_ENV with step-level env declarations.
# Normalize at the actual use site, in BOTH prepare and publish.
configure_r2_endpoint() {
  local endpoint="${R2_ENDPOINT%/}"
  endpoint="${endpoint%/${R2_BUCKET}}"
  [[ "$endpoint" =~ ^https://[^/]+$ ]] ||
    die "R2_ENDPOINT must be an account-level HTTPS S3 endpoint, optionally with /$R2_BUCKET"
  export RCLONE_CONFIG_R2_ENDPOINT="$endpoint"
  echo "RCLONE_CONFIG_R2_ENDPOINT=$endpoint" >> "$GITHUB_ENV"
  echo "Using account-level R2 endpoint: $endpoint"
}

case "${1:-}" in
  prepare)
    required=(R2_BUCKET R2_ENDPOINT R2_PUBLIC_URL R2_ACCESS_KEY_ID R2_SECRET_ACCESS_KEY FLATPAK_GPG_PRIVATE_KEY)
    missing=()
    for name in "${required[@]}"; do
      if [[ -z "${!name:-}" ]]; then missing+=("$name"); fi
    done
    if (( ${#missing[@]} )); then
      echo "enabled=false" >> "$GITHUB_OUTPUT"
      echo "::notice::R2 publication disabled. Set GitHub variables R2_BUCKET, R2_ENDPOINT, R2_PUBLIC_URL and secrets R2_ACCESS_KEY_ID, R2_SECRET_ACCESS_KEY, FLATPAK_GPG_PRIVATE_KEY. Missing: ${missing[*]}"
      exit 0
    fi

    [[ "$R2_ENDPOINT" == https://* ]] || die "R2_ENDPOINT must be an HTTPS S3 endpoint"
    [[ "$R2_PUBLIC_URL" == https://* ]] || die "R2_PUBLIC_URL must be an HTTPS public-bucket URL"

    configure_r2_endpoint
    export GNUPGHOME="${RUNNER_TEMP}/moonlight-flatpak-gnupg"
    mkdir -p "$GNUPGHOME"
    chmod 700 "$GNUPGHOME"
    printf '%s\n' "$FLATPAK_GPG_PRIVATE_KEY" | gpg --batch --import >/dev/null
    mapfile -t key_fingerprints < <(gpg --batch --with-colons --list-secret-keys | awk -F: '$1 == "fpr" { print $10 }')
    [[ "${#key_fingerprints[@]}" -eq 1 ]] || die "Expected exactly one Flatpak signing secret key"
    key_id="${key_fingerprints[0]}"

    echo "GNUPGHOME=$GNUPGHOME" >> "$GITHUB_ENV"
    echo "enabled=true" >> "$GITHUB_OUTPUT"
    echo "key_id=$key_id" >> "$GITHUB_OUTPUT"

    # Validate read AND write access before doing an expensive Flatpak build.
    # A tiny, unique probe avoids a repeat of the previous endless 501 uploads.
    probe_file="$RUNNER_TEMP/moonlight-r2-probe.txt"
    probe_object="r2:${R2_BUCKET}/ci-probe-${GITHUB_RUN_ID}-${GITHUB_RUN_ATTEMPT}.txt"
    printf '%s\n' "Moonlight R2 upload validation" > "$probe_file"
    rclone_checked copyto "$probe_file" "$probe_object" ||
      die "R2 write preflight failed: verify S3 endpoint and token permissions"
    rclone_checked cat "$probe_object" >/dev/null ||
      die "R2 read preflight failed"
    rclone_checked deletefile "$probe_object" ||
      die "R2 cleanup preflight failed"
    rm -f "$probe_file"
    echo "R2 upload preflight passed."

    # An empty R2 bucket is valid on the first build; subsequent runs restore
    # the previous OSTree objects so incremental static deltas can be generated.
    rclone_checked lsf "r2:${R2_BUCKET}/" --max-depth 1 >/dev/null
    mkdir -p repo
    rclone_checked copy "r2:${R2_BUCKET}/" repo/ --exclude '/moonlight-fork.flatpakrepo'
    echo "Existing R2 OSTree repository restored (empty on first publication)."
    ;;

  publish)
    [[ -n "${FLATPAK_GPG_KEY_ID:-}" ]] || die "Missing key fingerprint from prepare step"
    [[ -d repo ]] || die "Missing OSTree repository"
    configure_r2_endpoint
    flatpak build-update-repo \
      --generate-static-deltas \
      --gpg-sign="$FLATPAK_GPG_KEY_ID" \
      repo

    public_url="${R2_PUBLIC_URL%/}"
    public_key="$(gpg --batch --export "$FLATPAK_GPG_KEY_ID" | base64 -w 0)"
    [[ -n "$public_key" ]] || die "Failed to export GPG public key"
    cat > moonlight-fork.flatpakrepo <<EOF
[Flatpak Repo]
Title=Moonlight (mspeedo fork)
Comment=Moonlight Flatpak builds from feature/remote-sunshine-settings
Url=$public_url
Homepage=https://github.com/mspeedo/moonlight-qt
DefaultBranch=master
GPGKey=$public_key
EOF

    # Publish immutable content-addressed files first; mutable refs and signed
    # summary last. Do not delete old objects while clients may still use them.
    rclone_checked copy repo/ "r2:${R2_BUCKET}/" \
      --exclude '/summary' \
      --exclude '/summary.sig' \
      --exclude '/refs/**' \
      --exclude '/tmp/**' \
      --exclude '/state/**' \
      --exclude '*.lock'
    rclone_checked copy repo/refs/ "r2:${R2_BUCKET}/refs/"
    rclone_checked copyto repo/summary.sig "r2:${R2_BUCKET}/summary.sig"
    rclone_checked copyto repo/summary "r2:${R2_BUCKET}/summary"
    rclone_checked copyto moonlight-fork.flatpakrepo "r2:${R2_BUCKET}/moonlight-fork.flatpakrepo"

    # Public access must be separately enabled on the R2 bucket.
    curl --fail --silent --show-error --location --retry 3 --retry-delay 2 \
      "$public_url/summary" --output /dev/null \
      || die "Uploaded but public summary unreachable. Enable public bucket access and check R2_PUBLIC_URL."
    curl --fail --silent --show-error --location \
      "$public_url/moonlight-fork.flatpakrepo" --output /dev/null \
      || die "Public Flatpak remote descriptor is unreachable"

    {
      echo '## Flatpak R2 publication'
      echo ''
      echo "- Repository: $public_url"
      echo "- Remote descriptor: $public_url/moonlight-fork.flatpakrepo"
      echo '- Signed commits and repository summary: yes'
      echo '- Incremental OSTree history: preserved'
    } >> "$GITHUB_STEP_SUMMARY"
    ;;

  *)
    die "Usage: $0 prepare|publish"
    ;;
esac
