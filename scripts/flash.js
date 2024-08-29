const { SerialPort } = require("serialport");
const { ReadlineParser } = require("@serialport/parser-readline");
const fs = require("fs");

("use strict");

// Firmware values
let firmwareArray = null;
// const baudRate = 115200;
const baudRate = 921600;

const activationTime = 1000; // 3 seconds

const portName = "/dev/ttyUSB0"; // Change to the appropriate port name

const log = (data) => {
    console.log(new Date().toLocaleTimeString() + ": " + data);
};

async function delay(ms) {
    return new Promise((resolve) => {
        setTimeout(resolve, ms);
    });
}

function hex(number, length) {
    var str = number.toString(16).toUpperCase();
    while (str.length < length) str = "0" + str;
    return str;
}

function sws_wr_addr(addr, data) {
    // log(addr + ':['+data+']');
    let d = new Uint8Array(10); // word swire 10 bits = 10 bytes UART
    let h = new Uint8Array([
        0x5a,
        (addr >> 16) & 0xff,
        (addr >> 8) & 0xff,
        addr & 0xff,
        0x00,
    ]);
    let pkt = new Uint8Array((data.byteLength + 6) * 10);
    d[0] = 0x80; // start bit byte cmd swire = 1
    d[9] = 0xfe; // stop bit swire = 0
    h.forEach((el, n) => {
        let m = 0x80; // mask bit
        let idx = 1;
        do {
            if ((el & m) != 0) d[idx] = 0x80;
            else d[idx] = 0xfe;
            idx += 1;
            m >>= 1;
        } while (m != 0);
        pkt.set(d, n * 10);
        d[0] = 0xfe; // start bit next byte swire = 0
    });
    data.forEach((el, n) => {
        let m = 0x80; // mask bit
        let idx = 1;
        do {
            if ((el & m) != 0) d[idx] = 0x80;
            else d[idx] = 0xfe;
            idx += 1;
            m >>= 1;
        } while (m != 0);
        pkt.set(d, (n + 5) * 10);
    });
    // swire stop cmd = 0xff
    d.fill(0x80, 0, 9);
    pkt.set(d, (data.byteLength + 5) * 10);
    return pkt;
}

async function FlashByteCmd(cmd) {
    await serialController.write_raw(sws_wr_addr(0x0d, new Uint8Array([0x00]))); // cns low
    return await serialController.write_raw(
        sws_wr_addr(0x0c, new Uint8Array([cmd & 0xff, 0x01]))
    ); // Flash cmd + cns high
}
async function FlashWriteEnable() {
    // send flash cmd 0x06 write enable to flash
    return FlashByteCmd(0x06);
}
async function FlashWakeUp() {
    // send flash cmd 0xab to wakeup flash
    return FlashByteCmd(0xab);
}
async function FlashUnlock() {
    // send flash cmd 0x01 unlock flash
    await serialController.write_raw(sws_wr_addr(0x0d, new Uint8Array([0x00]))); // cns low
    await serialController.write_raw(sws_wr_addr(0x0c, new Uint8Array([0x01]))); // Flash cmd
    await serialController.write_raw(sws_wr_addr(0x0c, new Uint8Array([0x00]))); // Unlock all
    return await serialController.write_raw(
        sws_wr_addr(0x0c, new Uint8Array([0x00, 0x01]))
    ); // Unlock all + cns high
}
async function FlashEraseAll() {
    // send flash cmd 0x60 erase all flash
    return FlashByteCmd(0x60);
}
async function WriteFifo(addr, data) {
    // send all data to one register (no increment address - fifo mode)
    await serialController.write_raw(
        sws_wr_addr(0x00b3, new Uint8Array([0x80]))
    ); // [0xb3]=0x80 ext.SWS into fifo mode
    await serialController.write_raw(sws_wr_addr(addr, data)); // send all data to one register (no increment address - fifo mode)
    return await serialController.write_raw(
        sws_wr_addr(0x00b3, new Uint8Array([0x00]))
    ); // [0xb3]=0x00 ext.SWS into normal(ram) mode
}
async function SectorErase(addr) {
    await FlashWriteEnable();
    await serialController.write_raw(sws_wr_addr(0x0d, new Uint8Array([0x00]))); // cns low
    await serialController.write_raw(sws_wr_addr(0x0c, new Uint8Array([0x20]))); // Flash cmd erase sector
    await serialController.write_raw(
        sws_wr_addr(0x0c, new Uint8Array([(addr >> 16) & 0xff]))
    ); // Faddr hi
    await serialController.write_raw(
        sws_wr_addr(0x0c, new Uint8Array([(addr >> 8) & 0xff]))
    ); // Faddr mi
    await serialController.write_raw(
        sws_wr_addr(0x0c, new Uint8Array([addr & 0xff, 0x01]))
    ); // Faddr lo + cns high
    return await delay(300);
}
async function WriteFlashBlk(addr, data) {
    await FlashWriteEnable();
    await serialController.write_raw(sws_wr_addr(0x0d, new Uint8Array([0x00]))); // cns low
    let blk = new Uint8Array(4 + data.byteLength);
    blk[0] = 0x02;
    blk[1] = (addr >> 16) & 0xff;
    blk[2] = (addr >> 8) & 0xff;
    blk[3] = addr & 0xff;
    blk.set(data, 4);
    await WriteFifo(0x0c, blk); // send all data to SPI data register
    await serialController.write_raw(sws_wr_addr(0x0d, new Uint8Array([0x01]))); // cns high
    return await delay(10);
}
async function SoftResetMSU() {
    return await serialController.write_raw(
        sws_wr_addr(0x06f, new Uint8Array([0x20]))
    ); // Soft Reset MCU
}
async function Activate(tim) {
    let blk = sws_wr_addr(0x0602, new Uint8Array([0x05])); // CPU stop
    let s = "Reset DTR/RTS (100 ms)";
    log(s);
    await serialController.reset(100); // DTR & RTS
    console.log("Soft Reset MCU");
    await SoftResetMSU();
    s = "Activate (" + tim / 1000.0 + " sec)...";
    log(s);
    let t = new Date().getTime();
    while (new Date().getTime() - t < tim) {
        await serialController.write_raw(blk); // CPU stop
    }
    await serialController.write_raw(sws_wr_addr(0x00b2, new Uint8Array([55]))); // Set SWS Speed
    await serialController.write_raw(blk);
    return await FlashWakeUp();
}
//---------------------------------
async function FlashWrite() {
    let t = new Date().getTime();
    await Activate(activationTime);
    log("Write " + firmwareArray.byteLength + " bytes in to Flash...");
    let len = firmwareArray.byteLength;
    let addr = 0;
    let sblk = 256; // max spi-flash fifo = 256
    while (len > 0) {
        if ((addr & 0x0fff) == 0) {
            // let s =
            //     Math.floor((addr / (firmwareArray.byteLength * 1.0)) * 100) +
            //     "% Flash Sector Erase at 0x" +
            //     hex(addr, 6);
            // console.log(s);
            await SectorErase(addr);
        }
        if (len < sblk) sblk = len;
        let s =
            Math.floor((addr / (firmwareArray.byteLength * 1.0)) * 100) +
            "% Flash Write " +
            sblk +
            " bytes at 0x" +
            hex(addr, 6);
        // console.log(s);
        process.stdout.write(s + "\r");
        await WriteFlashBlk(
            addr,
            new Uint8Array(firmwareArray.slice(addr, addr + sblk))
        );
        addr += sblk;
        len -= sblk;
    }
    log("Done (" + (new Date().getTime() - t) / 1000.0 + " sec).");
    log("Soft Reset MCU");
    return await SoftResetMSU();
}
async function DeviceStop() {
    return serialController.close();
}
const writeFlash = async function () {
    await FlashWrite();
};
const unlockFlash = async function () {
    await Activate(activationTime);

    log("Flash Erase All (3.5 sec)...");
    await FlashWriteEnable();
    await FlashUnlock();
    await delay(3500);
    log("Done.");
};
const eraseFlash = async function () {
    await Activate(activationTime);
    log("Flash Erase All (3.5 sec)...");
    await FlashWriteEnable();
    await FlashEraseAll();
    await delay(3500);
    log("Done.");
};
const resetMCU = async function () {
    await Activate(activationTime);
    log("Soft Reset MCU");
    await SoftResetMSU();
    log("Done.");
};

//---------------------------------
class SerialController {
    port = null;

    async init(init_cb) {
        await new Promise((res, rej) => {
            this.port = new SerialPort(
                {
                    path: portName,
                    baudRate: baudRate,
                },
                res
            );
            this.port.pipe(new ReadlineParser());
            this.port.set({
                dts: true,
                rts: true,
            });
            this.port.on("error", function (err) {
                console.log("Error:", err);
            });
        });

        // Switches the port into "flowing mode"
        // this.port.on("data", function (data) {
        //     console.log(data.toString());
        //     log("Data:", data);
        // });

        // if ("serial" in navigator) {
        //     try {
        //         this.port = await navigator.serial.requestPort();
        //         await this.port.open({
        //             baudRate: ubaud.value,
        //             bufferSize: 240,
        //             buffersize: 240,
        //         }); // % 10, 60..n <= USB-COM Chip fifo
        //         this.writer = this.port.writable.getWriter();
        //         console.log("DTR, RTS on.");
        //         await this.port.setSignals({
        //             dataTerminalReady: false,
        //             requestToSend: false,
        //         });
        //         if (typeof init_cb == "function") await init_cb(this.port);
        //     } catch (err) {
        //         log("There was an error opening the serial port: " + err);
        //     }
        // } else {
        //     log(
        //         "Web serial doesn't seem to be enabled in your browser. Try enabling it by visiting:"
        //     );
        //     log("chrome://flags/#enable-experimental-web-platform-features");
        //     log("opera://flags/#enable-experimental-web-platform-features");
        //     log("edge://flags/#enable-experimental-web-platform-features");
        // }
    }
    async write_raw(data) {
        return new Promise((res, rej) => {
            this.port.write(data, function (err) {
                if (err) {
                    log("Error on write: ", err.message);
                    rej(err);
                    return;
                }
                res();
            });
        });
        // return await this.writer.write(data);
    }
    async reset(t_ms) {
        console.log("DTR, RTS on.");
        await this.port.set({
            dts: true,
            rts: true,
        });
        await delay(t_ms);
        console.log("DTR, RTS off.");
        return await this.port.set({
            dts: true,
            rts: false,
        });
    }
    async close() {
        await this.port.close();
        log("USB-COM closed.");
    }
}

const args = process.argv.slice(2);

const fileName = args[0];
log(`Loading file ${fileName}`);
let serialController;

const start = async () => {
    firmwareArray = fs.readFileSync(fileName);

    firmwareArray = new Uint8Array(firmwareArray);

    serialController = new SerialController();
    await serialController.init();

    await unlockFlash();
    await FlashEraseAll();
    await FlashWrite();

    // await FlashEraseAll();
    // await FlashWrite();
    await serialController.close();
    process.exit();
};

start();

// if (
//     firmwareArray.byteLength < 16 ||
//     new Uint32Array(firmwareArray.slice(8, 12)) != [0x54, 0x4c, 0x4e, 0x4b]
// ) {
//     log("Select file is no telink firmware .bin");
//     firmwareArray = null;
//     return;
// }
