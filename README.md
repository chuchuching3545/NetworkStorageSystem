# NetworkStorageSystem

## 簡介

模擬雲端儲存系統、youtube影片播放，client可以在連上server之後，輸入指令來存取資料或播放影片。

## client指令

```bash=
$ ls
```
* 查看server資料夾中所有檔案名

```bash=
$ put [file_name]
```
* 上傳檔案到server資料夾中

```bash=
$ get [file_name]
```
* 下載檔案到client資料夾中

```bash=
$ play [video_name].mpg
```
* 接收來自server的encoded frame，並在client端decode及播放，過程中不會下載影片。
- 支援buffer機制，類似youtube影片時間軸中灰色的部分。

## Run

* 建立server
	* 輸入`Make server`
	- 輸入`./server [port]`
		* port是server的port

- 建立client
	* 輸入`Make client`
	- 輸入`./client [IP]:[port]`
		* IP是server的IP
		- port是server的port
	+ 可以同時建立許多client
		
