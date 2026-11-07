#!/usr/bin/env bash
# Publish the fork's Flatpak OSTree repository to Cloudflare R2.
set -euo pipefail

die() { echo "::error::$*" >&2; exit 1; }

: "${GITHUB_OUTPUT:=/dev/null}"

# Avoid infinite retries on authentication, API or S3 endpoint errors.
rclone_checked() {
  # Cloudflare R2 may return temporary 503s when many objects are written.
  # Retry with backoff, limit request rate, and show periodic transfer stats.
  timeout --signal=TERM 1200s rclone --s3-no-head \
    --retries 5 --low-level-retries 3 --retries-sleep 8s \
    --transfers 4 --checkers 8 --tpslimit 12 \
    --contimeout 12s --timeout 45s --stats 30s --stats-one-line "$@"
}

# The next GitHub Actions step overrides GITHUB_ENV with step-specific env.
# Re-normalize the S3 URL in BOTH prepare and publish.
configure_r2_endpoint() {
  local endpoint="${R2_ENDPOINT%/}"
  endpoint="${endpoint%/${R2_BUCKET}}"
  [[ "$endpoint" =~ ^https://[^/]+$ ]] ||
    die "R2_ENDPOINT must be an account-level HTTPS S3 endpoint, optionally followed by /$R2_BUCKET"
  export RCLONE_CONFIG_R2_ENDPOINT="$endpoint"
  # Bucket-scoped Object-level API tokens may not call HeadBucket.
  export RCLONE_CONFIG_R2_NO_CHECK_BUCKET=true
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

    # Validate R2 write, read and delete permissions before compiling.
    probe_file="$RUNNER_TEMP/moonlight-r2-probe.txt"
    # Keep all new releases under a clean prefix. Old failed root-level
    # uploads contained a file and directory with the same name.
    repo_remote="r2:${R2_BUCKET}/flatpak/"
    probe_object="${repo_remote}ci-probe-${GITHUB_RUN_ID}-${GITHUB_RUN_ATTEMPT}.txt"
    printf '%s\n' "Moonlight R2 upload validation" > "$probe_file"
    rclone_checked copyto "$probe_file" "$probe_object" ||
      die "R2 write preflight denied. Check Object Read & Write permission, account and bucket scope."
    rclone_checked cat "$probe_object" >/dev/null ||
      die "R2 read preflight denied."
    rclone_checked deletefile "$probe_object" ||
      die "R2 delete preflight denied."
    rm -f "$probe_file"
    echo "R2 upload preflight passed."

    # No R2 restore: flatpak-builder creates a fresh local OSTree repository.
    # Existing content-addressed objects remain on R2 between releases.
    echo "publish_ready=true" >> "$GITHUB_OUTPUT"
    echo "R2 upload verified; no repository download is necessary."
    ;;

  publish)
    [[ -n "${FLATPAK_GPG_KEY_ID:-}" ]] || die "Missing key fingerprint from prepare step"
    [[ -d repo ]] || die "Missing OSTree repository"
    configure_r2_endpoint
    repo_remote="r2:${R2_BUCKET}/flatpak/"
    # Sign the repository index; no slow static-delta generation.
    flatpak build-update-repo \
      --gpg-sign="$FLATPAK_GPG_KEY_ID" \
      repo

    public_url="${R2_PUBLIC_URL%/}/flatpak"
    public_key="$(gpg --batch --export "$FLATPAK_GPG_KEY_ID" | base64 -w 0)"
    [[ -n "$public_key" ]] || die "Failed to export GPG public key"
    cat > moonlight-fork.flatpakrepo <<EOF
[Flatpak Repo]
Title=Moonlight (mspeedo fork)
Comment=Moonlight Flatpak builds from feature/flatpak-update-page
Url=$public_url
Homepage=https://github.com/mspeedo/moonlight-qt
DefaultBranch=master
GPGKey=$public_key
EOF

    # Only upload missing immutable OSTree objects. Existing objects use
    # content-hash paths and are safely reusable across independent builds.
    # --fast-list avoids per-directory S3 listing round trips.
    rclone_checked copy repo/objects/ "${repo_remote}objects/" \
      --ignore-existing --fast-list

    # Upload repository metadata, without duplicating objects or exposing
    # the new signed summary until all commit objects and refs are present.
    rclone_checked copy repo/ "$repo_remote" \
      --fast-list \
      --exclude '/objects/**' \
      --exclude '/summary' \
      --exclude '/summary.sig' \
      --exclude '/refs/**' \
      --exclude '/tmp/**' \
      --exclude '/state/**' \
      --exclude '*.lock'
    rclone_checked copy repo/refs/ "${repo_remote}refs/"
    rclone_checked copyto repo/summary.sig "${repo_remote}summary.sig"
    rclone_checked copyto repo/summary "${repo_remote}summary"
    rclone_checked copyto moonlight-fork.flatpakrepo "${repo_remote}moonlight-fork.flatpakrepo"

    # Publish this index LAST, after all signed OSTree data is available.
    # Moonlight uses it for a real on-demand check rather than waiting for
    # the portal's periodic (typically 30-minute) remote polling cycle.
    ostree_commit="$(ostree --repo=repo rev-parse app/com.moonlight_stream.Moonlight/x86_64/master)"
    [[ "$ostree_commit" =~ ^[a-f0-9]{64}$ ]] || die "Could not resolve the published Moonlight OSTree commit"
    published_at="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    cat > moonlight-latest.json <<EOF
{"schema":1,"app_id":"com.moonlight_stream.Moonlight","arch":"x86_64","branch":"master","ostree_commit":"$ostree_commit","git_commit":"$GITHUB_SHA","published_at":"$published_at"}
EOF
    rclone_checked copyto moonlight-latest.json "${repo_remote}moonlight-latest.json"

    # Public access must be separately enabled on the R2 bucket.
    curl --fail --silent --show-error --location --retry 3 --retry-delay 2 \
      "$public_url/summary" --output /dev/null \
      || die "Uploaded but public summary unreachable. Enable public bucket access and check R2_PUBLIC_URL."
    curl --fail --silent --show-error --location \
      "$public_url/moonlight-fork.flatpakrepo" --output /dev/null \
      || die "Public Flatpak remote descriptor is unreachable"
    curl --fail --silent --show-error --location \
      "$public_url/moonlight-latest.json" --output /dev/null \
      || die "Public release index is unreachable"

    {
      echo '## Flatpak R2 publication'
      echo ''
      echo "- Repository: $public_url"
      echo "- Remote descriptor: $public_url/moonlight-fork.flatpakrepo"
      echo '- Signed commits and repository summary: yes'
      echo '- Existing OSTree objects: preserved; no remote restore or static deltas'
      echo "- Latest release index: $public_url/moonlight-latest.json"
    } >> "$GITHUB_STEP_SUMMARY"
    ;;

  *)
    die "Usage: $0 prepare|publish"
    ;;
esac
