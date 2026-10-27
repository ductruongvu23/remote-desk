#!/bin/bash
# Build app iPhone Remote Desk (cần Mac có Xcode 16 trở lên).
#   bash ios/build.sh          -> ios/build/RemoteDesk-unsigned.ipa (ký và cài bằng Sideloadly như WDA.ipa)
#   bash ios/build.sh --test   -> chạy self-test trên iOS Simulator
# Ký bằng Apple ID trong Xcode: mở ios/RemoteDesk.xcodeproj, chọn Team ở Signing & Capabilities,
# cắm iPhone rồi bấm Run, hoặc Product > Archive > Distribute App để xuất .ipa.
set -euo pipefail
cd "$(dirname "$0")"
rm -rf build
mkdir -p build

# xcodebuild; lỗi thì in các dòng error ra chú thích và phần tóm tắt của GitHub Actions
# (xem được trên trang Actions mà không cần đăng nhập, khác với log đầy đủ).
xcb() {
  if xcodebuild "$@" > build/xcodebuild.log 2>&1; then return 0; fi
  grep -E "error:" build/xcodebuild.log | sed "s|$PWD/||" | sort -u > build/errors.txt || true
  head -10 build/errors.txt | while IFS= read -r l; do echo "::error::$l"; done
  if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then
    { echo '### Lỗi biên dịch'; echo '```'; head -80 build/errors.txt; echo '```'; } >> "$GITHUB_STEP_SUMMARY"
  fi
  tail -40 build/xcodebuild.log
  return 1
}

if [ "${1:-}" = "--test" ]; then
  xcb -quiet -project RemoteDesk.xcodeproj -target RemoteDesk -configuration Debug -sdk iphonesimulator \
    SYMROOT="$PWD/build" CODE_SIGNING_ALLOWED=NO build
  UDID=$(xcrun simctl list devices available -j | python3 -c 'import json, sys
d = json.load(sys.stdin)["devices"]
print(next(x["udid"] for r in d if "iOS" in r for x in d[r] if "iPhone" in x["name"]))')
  xcrun simctl boot "$UDID" 2>/dev/null || true
  xcrun simctl install "$UDID" build/Debug-iphonesimulator/RemoteDesk.app
  xcrun simctl launch --console "$UDID" vn.vdt.remotedesk --self-test | tee build/selftest.log
  if ! grep -q "self-test OK" build/selftest.log; then  # in các dòng lỗi (hoặc cuối log nếu app văng) ra chú thích
    { grep "SELF-TEST" build/selftest.log || tail -15 build/selftest.log; } | head -30 | while IFS= read -r l; do echo "::error::$l"; done
    sleep 5  # chờ Simulator ghi báo cáo crash
    crash=$(ls -t ~/Library/Logs/DiagnosticReports/RemoteDesk*.ips 2>/dev/null | head -1 || true)
    if [ -n "$crash" ]; then  # app văng: loại lỗi và các khung stack của luồng hỏng
      python3 - "$crash" <<'PY' | while IFS= read -r l; do echo "::error::crash: $l"; done
import json, sys
body = json.loads(open(sys.argv[1]).read().split("\n", 1)[1])
print(json.dumps(body.get("exception", {})), body.get("termination", {}).get("indicator", ""))
imgs = body.get("usedImages", [])
for f in body["threads"][body.get("faultingThread", 0)]["frames"][:14]:
    name = imgs[f["imageIndex"]].get("name", "?") if f.get("imageIndex", -1) < len(imgs) else "?"
    print(name, f.get("symbol", hex(f.get("imageOffset", 0))), f.get("sourceFile", ""), f.get("sourceLine", ""))
PY
    fi
    exit 1
  fi
  exit
fi

xcb -quiet -project RemoteDesk.xcodeproj -target RemoteDesk -configuration Release -sdk iphoneos \
  SYMROOT="$PWD/build" CODE_SIGNING_ALLOWED=NO build
mkdir -p build/ipa/Payload
cp -R build/Release-iphoneos/RemoteDesk.app build/ipa/Payload/
(cd build/ipa && zip -qr ../RemoteDesk-unsigned.ipa Payload)
echo "Xong: ios/build/RemoteDesk-unsigned.ipa"
