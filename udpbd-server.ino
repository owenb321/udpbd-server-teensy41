#include <iostream>
#include <exception>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "udpbd.h"
#include <QNEthernet.h>

#include <SD.h>
#include <USBHost_t36.h>

using namespace std;
using namespace qindesign::network;

// UDP queue
EthernetUDP udp(32);

SdioCard sdcard;

USBHost usb;
USBHub hub1(usb);
USBDrive drive(usb);

const int ledPin = 13;

class CBlockDevice {
public:
  CBlockDevice()
    : _current_sector(0), _leftover_bytes(0), _read_started(0), _write_started(0), _sdmode(0) {}

  bool init_sd() {
    Serial.println("Mounting SD card...");
    if (!sdcard.begin(SdioConfig(DMA_SDIO))) {
      Serial.println("ERROR: SD begin failed");
      return false;
    }

    _sdmode = true;

    // Get the size of the card (doesn't seem accurate?)
    _fsize = sdcard.sectorCount() * 512;

    Serial.print("Opened SD card as Block Device");
    Serial.print(" - size = ");
    Serial.print(_fsize / (1000 * 1000));
    Serial.print("MB / ");
    Serial.print(_fsize / (1024 * 1024));
    Serial.println("MiB");
    return true;
  }

  bool init_usb() {
    Serial.println("Mounting USB drive...");
    usb.begin();
    if (!drive.begin()) {
      Serial.println("ERROR: USB begin failed");
      return false;
    }
    drive.mscInit();
    drive.startFilesystems();
    // Wait for the drive to start.
    while (!drive.filesystemsStarted()) {
      usb.Task();
    }

    // Get the size of the drive
    _fsize = drive.msDriveInfo.capacity.Blocks;
    _fsize *= drive.msDriveInfo.capacity.BlockSize;

    Serial.print("Opened USB drive as Block Device");
    Serial.print(" - size = ");
    Serial.print(_fsize / (1000 * 1000));
    Serial.print("MB / ");
    Serial.print(_fsize / (1024 * 1024));
    Serial.println("MiB");
    return true;
  }

  ~CBlockDevice() {
  }

  void seek(uint32_t sector) {
    _current_sector = sector;
    _leftover_bytes = 0;
  }

  void read(uint8_t *data, size_t size) {
    size_t total_bytes_read = 0;

    if (_leftover_bytes) {
      memcpy(data, _leftover_buffer, _leftover_bytes);
      total_bytes_read += _leftover_bytes;
    }

    // Get ceiling of size / sector_size
    size_t sectors_to_read = (size - _leftover_bytes + 511) / 512;

    if (_sdmode) {
      if (!sdcard.readSectors(_current_sector, data + total_bytes_read, sectors_to_read)) {
        Serial.print("SD readSectors error");
      }
    } else {
      if (!drive.readSectors(_current_sector, data + total_bytes_read, sectors_to_read)) {
        Serial.print("USB readSectors error");
      }
    }
    total_bytes_read += sectors_to_read * 512;
    _current_sector += sectors_to_read;
    _leftover_bytes = total_bytes_read - size;

    if (_leftover_bytes) {
      memcpy(_leftover_buffer, data + total_bytes_read - _leftover_bytes, _leftover_bytes);
    }
  }

  void write(uint8_t *data, size_t size) {
    size_t bytes_to_write = size + _leftover_bytes;

    if (_leftover_bytes) {
      memmove(data + _leftover_bytes, data, size);
      memcpy(data, _leftover_buffer, _leftover_bytes);
    }

    // Get floor of size / sector_size
    size_t sectors_to_write = bytes_to_write / 512;
    if (_sdmode) {
      if (!sdcard.writeSectors(_current_sector, data, sectors_to_write)) {
        Serial.print("SD write error");
      }
    } else {
      if (!drive.writeSectors(_current_sector, data, sectors_to_write)) {
        Serial.print("USB write error");
      }
    }
    _current_sector += sectors_to_write;
    _leftover_bytes = bytes_to_write - (sectors_to_write * 512);
    if (_leftover_bytes) {
      memcpy(_leftover_buffer, data + (sectors_to_write * 512), _leftover_bytes);
    }
  }

  uint32_t get_sector_size() {
    return 512;
  }
  uint32_t get_sector_count() {
    return _fsize / 512;
  }

private:
  bool _sdmode;
  uint64_t _fsize;
  uint32_t _current_sector;
  size_t _leftover_bytes;
  bool _read_started;
  bool _write_started;
  uint8_t _leftover_buffer[512];
};
class CBlockDevice bd;

/*
 * class CUDPBDServer
 */
class CUDPBDServer {
public:
  CUDPBDServer()
    : _block_shift(0), _total_read(0), _total_write(0) {}

  void init() {
    set_block_shift(5);  // 128b blocks
    udp.begin(UDPBD_PORT);
  }

  ~CUDPBDServer() {
    udp.stop();
  }

  void run() {
    // Receive command from ps2
    int packetSize = udp.parsePacket();
    if (packetSize < 0) {
      return;
    }
    IPAddress remoteIp = udp.remoteIP();
    uint16_t remotePort = udp.remotePort();

    struct SUDPBDv2_Header *hdr = (struct SUDPBDv2_Header *)udp.data();

    // Process command
    switch (hdr->cmd) {
      case UDPBD_CMD_INFO:
        handle_cmd_info(remoteIp, remotePort, (struct SUDPBDv2_InfoRequest *)udp.data());
        break;
      case UDPBD_CMD_READ:
        handle_cmd_read(remoteIp, remotePort, (struct SUDPBDv2_RWRequest *)udp.data());
        break;
      case UDPBD_CMD_WRITE:
        handle_cmd_write(remoteIp, remotePort, (struct SUDPBDv2_RWRequest *)udp.data());
        break;
      case UDPBD_CMD_WRITE_RDMA:
        handle_cmd_write_rdma(remoteIp, remotePort, (struct SUDPBDv2_RDMA *)udp.data());
        break;
      default:
        Serial.print("Invalid cmd: 0x");
        Serial.println(hdr->cmd, HEX);
    };
  }

private:
  void print_stats() {
    Serial.print("Total read: ");
    Serial.print(_total_read / 1024);
    Serial.print(" KiB, total write: ");
    Serial.print(_total_write / 1024);
    Serial.println(" KiB");
    fflush(stdout);
  }

  void set_block_shift(uint32_t shift) {
    if (shift != _block_shift) {
      _block_shift = shift;
      _block_size = 1 << (_block_shift + 2);
      _blocks_per_packet = RDMA_MAX_PAYLOAD / _block_size;
      _blocks_per_sector = bd.get_sector_size() / _block_size;
      Serial.print("Block size changed to ");
      Serial.println(_block_size);
    }
  }

  void set_block_shift_sectors(uint32_t sectors) {
    // Optimize for:
    // 1 - the least number of network packets
    // 2 - the largest block size (faster on the ps2)
    uint32_t shift;
    uint32_t size = sectors * 512;
    uint32_t packetsMIN = (size + 1440 - 1) / 1440;
    uint32_t packets128 = (size + 1408 - 1) / 1408;
    uint32_t packets256 = (size + 1280 - 1) / 1280;
    uint32_t packets512 = (size + 1024 - 1) / 1024;

    if (packets512 == packetsMIN)
      shift = 7;  // 512 byte blocks
    else if (packets256 == packetsMIN)
      shift = 6;  // 256 byte blocks
    else if (packets128 == packetsMIN)
      shift = 5;  // 128 byte blocks
    else
      shift = 3;  //  32 byte blocks

    set_block_shift(shift);
    //set_block_shift(7);
  }

  void handle_cmd_info(IPAddress remoteip, uint16_t remotePort, struct SUDPBDv2_InfoRequest *request) {
    digitalWrite(ledPin, HIGH);
    struct SUDPBDv2_InfoReply reply;

    char buffer[40];
    sprintf(buffer, "UDPBD_CMD_INFO from %u.%u.%u.%u", remoteip[0], remoteip[1], remoteip[2], remoteip[3]);
    Serial.println(buffer);
    print_stats();

    // // Reply header
    reply.hdr.cmd = UDPBD_CMD_INFO_REPLY;
    reply.hdr.cmdid = request->hdr.cmdid;
    reply.hdr.cmdpkt = 1;
    // // Reply info
    reply.sector_size = bd.get_sector_size();
    reply.sector_count = bd.get_sector_count();

    // Send packet to ps2
    if (!udp.send(remoteip, remotePort, (char *)&reply, sizeof(reply))) {
      Serial.println("ERROR: send");
      exit(1);
    }
    digitalWrite(ledPin, LOW);
  }

  void handle_cmd_read(IPAddress remoteip, uint16_t remotePort, struct SUDPBDv2_RWRequest *request) {
    digitalWrite(ledPin, HIGH);
    struct SUDPBDv2_RDMA reply;

    Serial.print("\rUDPBD_CMD_READ(cmdId=");
    Serial.print(request->hdr.cmdid);
    Serial.print(", startSector=");
    Serial.print(request->sector_nr);
    Serial.print(", sectorCount=");
    Serial.print(request->sector_count);
    Serial.println(")");
    // Optimize RDMA block size for number of sectors
    set_block_shift_sectors(request->sector_count);

    // // Reply header
    reply.hdr.cmd = UDPBD_CMD_READ_RDMA;
    reply.hdr.cmdid = request->hdr.cmdid;
    reply.hdr.cmdpkt = 1;
    reply.bt.block_shift = _block_shift;

    uint32_t blocks_left = request->sector_count * _blocks_per_sector;

    _total_read += blocks_left * _block_size;
    // print_stats();

    bd.seek(request->sector_nr);

    while (blocks_left > 0) {
      reply.bt.block_count = (blocks_left > _blocks_per_packet) ? _blocks_per_packet : blocks_left;
      blocks_left -= reply.bt.block_count;

      bd.read(reply.data, reply.bt.block_count * _block_size);

      // Send packet to ps2
      if (!udp.send(remoteip, remotePort, (char *)&reply, sizeof(struct SUDPBDv2_Header) + 4 + (reply.bt.block_count * _block_size))) {
        Serial.println("ERROR: send");
        exit(1);
      }
      reply.hdr.cmdpkt++;
    }
    digitalWrite(ledPin, LOW);
  }

  void handle_cmd_write(IPAddress remoteip, uint16_t remotePort, struct SUDPBDv2_RWRequest *request) {
    Serial.print("\rUDPBD_CMD_WRITE(cmdId=");
    Serial.print(request->hdr.cmdid);
    Serial.print(", startSector=");
    Serial.print(request->sector_nr);
    Serial.print(", sectorCount=");
    Serial.print(request->sector_count);
    Serial.println(")");

    bd.seek(request->sector_nr);
    _write_size_left = request->sector_count * 512;

    _total_write += _write_size_left;
    // print_stats();
  }

  void handle_cmd_write_rdma(IPAddress remoteip, uint16_t remotePort, struct SUDPBDv2_RDMA *request) {
    digitalWrite(ledPin, HIGH);
    size_t size = request->bt.block_count * (1 << (request->bt.block_shift + 2));
    // printf("UDPBD_CMD_WRITE_RDMA(cmdId=%d, BS=%d, BC=%d, size=%ld)\n", request->hdr.cmdid, request->bt.block_shift, request->bt.block_count, size);

    bd.write(request->data, size);
    _write_size_left -= size;

    if (_write_size_left == 0) {
      struct SUDPBDv2_WriteDone reply;

      // // Reply header
      reply.hdr.cmd = UDPBD_CMD_WRITE_DONE;
      reply.hdr.cmdid = request->hdr.cmdid;
      reply.hdr.cmdpkt = request->hdr.cmdid + 1;
      reply.result = 0;

      // Send packet to ps2
      if (!udp.send(remoteip, remotePort, (char *)&reply, sizeof(reply))) {
        Serial.println("ERROR: send");
        exit(1);
      }
    }
    digitalWrite(ledPin, LOW);
  }

  uint32_t _block_shift;
  uint32_t _block_size;
  uint32_t _blocks_per_packet;
  uint32_t _blocks_per_sector;

  uint64_t _total_read;
  uint64_t _total_write;

  uint32_t _write_size_left;
};
class CUDPBDServer srv;

void setup() {
  Serial.begin(9600);
  Serial.println("Boot");

  pinMode(ledPin, OUTPUT);

  IPAddress staticIP{ 192, 168, 1, 2 };
  IPAddress subnetMask{ 255, 255, 255, 0 };
  IPAddress gateway{ 192, 168, 1, 1 };

  printf("Starting Ethernet with static IP (%u.%u.%u.%u)...\n", staticIP[0], staticIP[1], staticIP[2], staticIP[3]);
  if (!Ethernet.begin(staticIP, subnetMask, gateway)) {
    Serial.println("Failed to start Ethernet");
    return;
  }
  // Try SD first then USB
  if (!bd.init_sd() && !bd.init_usb()) {
    Serial.println("Unable to mount SD or USB!");
    exit(1);
  }
  srv.init();
}

void loop() {
  srv.run();
}