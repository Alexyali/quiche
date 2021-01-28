
# Build **hevc-pipe** for 4K video transmission
- Key idea: We use **ffmpeg** to generate emulate live 4K video streaming, then push stream into **quiche_server**. Server sends data into network, and **quiche_client** receives them. Client then delivers video to **ffplay**, and shows video in the screen.
- We use [quiche](https://github.com/cloudflare/quiche.git) as the implement of QUIC protocols
- `hevc-pipe` use pipe to deliever video data between encoder and quiche protocol
- System: Ubuntu 18.04 LTS

## Download
- git clone repo
- switch branch to `hecv-pipe`
```shell
$ cd ~
$ git clone https://github.com/Alexyali/quic_hevc.git
$ cd quic_hevc/
$ git checkout hevc-pipe
```

## Dependencies
- install rust
```shell
$ sudo apt install curl
$ curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
$ source $HOME/.cargo/env
```

- install libev
```shell
$ sudo apt-get update -y
$ sudo apt-get install -y libev-dev
```

- install uthash
```shell
$ sudo apt-get install uthash-dev
```

- install golang
```shell
$ sudo apt install golang
```

- install cargo
```shell
$ sudo apt install cargo
```


## Build
- open terminal in `quic_hevc/`
- download `boringssl` in `quic_hevc/deps`, skip this command if deps/boringssl/ is not empty
```shell
$ cd ~/quic_hevc/deps/
$ rm -rf boringssl/
$ git clone https://github.com/google/boringssl.git
```

- build `quiche`
```shell
$ cd ~/quic_hevc/
$ cargo build --examples
```

- build `examples`
```shell
$ cd ~/quic_hevc/examples/
$ make clean
$ make
```

## Run **hevc-pipe**
- this step is to test if connnection can be estabished
- open terminal in `quic_hevc/examples`
- create `pipe` for send and receive data
- if successed, it will establish connection and transmit some packets
```shell
$ mkfifo svideopipe
$ mkfifo cvideopipe
$ mkfifo saudiopipe
$ mkfifo caudiopipe
$ ./server 127.0.0.1 1234
$ ./client 127.0.0.1 1234
```

## Test **hevc-pipe** with video stream
- you should first prepare a video file, such as ` demo.ts` in `quic_hevc/examples/`
- you should open **4 terminals** in `quic_hevc/examples/` to run **4 cmds** as shown below
- first open `ffplay` to receive data from cvideopipe, use`-infbuf` for live stream, and `-probesize 32` to decrease first open time
- then start `server`
- then use `client`, and use `ffmpeg` to push stream to pipe as fast as possible, otherwise timeout expired
```shell
$ ffplay -i cvideopipe -infbuf -probesize 32
$ ./server 127.0.0.1 1234
$ ./client 127.0.0.1 1234
$ ffmpeg -re -i ~/demo.ts -codec copy -f mpegts pipe:1 > svideopipe
```

### How client interact with server
- first run 4 cmds as shown in `Test hevc-pipe with video stream`
- type any Letters on the keyboard from *a* to *z* in `client` terminal, then client will send a message back to server
- in `server` terminal, you will see output like: `received 100 bits from client in stream 12 :*message s from client to server*`
