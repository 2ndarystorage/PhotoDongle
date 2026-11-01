# BLE Photo Dongle — ESP32-S3-0.96-SD ファームウェア

iPhoneアプリ「PhotoDongle」からBLEで写真を受け取り、microSDへ保存して
0.96インチLCD（160×80）にスライドショー表示するファームウェア（ESP-IDF）。

## ハードウェア

ESP32-S3-0.96-SD（Spotpear/GNPE、ESP32-S3N4R2・フラッシュ4MB）

- LCD（SPI2）：MOSI=4 / SCK=5 / CS=8 / DC=6 / RST=7 / BL=3、160×80、販売者初期化シーケンス
- SD（SPI3・SPIモード、20MHz）：MOSI=12 / MISO=14 / SCK=13 / CS=11
- BOOTボタン＝GPIO9：押すと次の写真へスキップ（RESETボタンはEN直結のため操作ボタンには使えない）

## 使い方

1. FAT32のmicroSDを挿してから電源を入れる（SDのマウントは起動時のみ）
2. iPhoneアプリから接続（BLE名「PHOTO-DONGLE」）して写真を送信
3. 受信した写真は `PHOTO_000.565` のような名前でSDに保存され、設定した間隔で順次（またはランダムに）表示される

## BLEプロトコル（GATTサービス 0xFFF0）

| Char | UUID | 方向 | 内容 |
|---|---|---|---|
| CTRL | FFF1 | Write | コマンド（下記） |
| DATA | FFF2 | Write | フレームデータ（受信中のみ有効） |
| STAT | FFF3 | Read/Notify | `[u8 状態][u16 枚数][u16 表示中番号][u16 間隔秒][u8 順序][u8 サイズ]` |

CTRLコマンド：

- `[0x01][u32 LE 総バイト数]` BEGIN：受信開始（総バイト数は25600固定）
- `[0x02]` COMMIT：保存を確定し `PHOTO_xxx.565` へリネーム
- `[0x03][u16 LE 間隔秒][u8 順序 0=順番/1=ランダム][u8 サイズ 0=フィット/1=拡大/2=縮小]` SETTINGS：NVSに保存
- `[0x04]` CLEAR：全写真削除
- `[0x05]` NEXT：次の写真へ（VJ再生中はVJを終了）
- `[0x06]` VJ_BEGIN：VJフレームを全消去し、以降のBEGIN/COMMITをVJ用フォルダ `/vj` へ振り向ける
- `[0x07]` VJ_PLAY：受信を終えてVJループ再生を開始
- `[0x08]` VJ_STOP：VJを止めてスライドショーへ戻る
- `[0x09]` VJ_RESUME：保存済みVJフレームを再び再生

フレーム形式：160×80、RGB565ビッグエンディアン、行優先、**25600バイト固定**。
画像の変換（リサイズ/トリミング）とVJフレームの生成はiPhone側で行う。サイズ設定は変換時にアプリ側で適用される
（間隔と順序はドングル側でも保持）。

状態コード：0=待機、1=受信中、2=エラー、4=VJ再生中。

VJモード：アプリが生成した効果フレーム（最大64枚）を `/vj/F000.565`… として保存し、
約8fpsでループ再生する（バックライトの明滅付き）。BOOTボタンでも終了できる。

色について：LCD初期化のMADCTLは販売者シーケンスの0xA8（BGR順）だとRGB565データの
赤と青が入れ替わるため、0xA0（RGB順）に変更している。

## ビルドと書き込み

```sh
. /Volumes/M2NVMe/work/esp/esp-idf/export.sh   # MacのESP-IDF
cd esp32-s3-photo-dongle
idf.py build
idf.py -p /dev/cu.usbmodem21101 flash monitor
```

- VM側ESP-IDF v5.4.2でビルド確認済み（app約0.65MB、2MB factoryパーティション）
- フラッシュは4MB前提（16MB設定にしないこと）
- シリアルポートを他のモニタが掴んでいると書き込みが排他ロックエラーになる

## 実装メモ

- 受信は一時ファイル `.recv.tmp` に書き、COMMIT時にリネーム（途中切断では写真にならない）
- 表示タスクはイベント駆動：表示間隔タイマ／BOOTボタン割り込み／BLEのNEXT・設定変更で即時反映
- SDアクセスはミューテックスで直列化（BLE受信と表示読み込みの競合防止）
