# /// script
# requires-python = ">=3.12"
# dependencies = ["smpclient[serial]==7.3.0"]
# ///
"""Updates both cores of an AudioLink over USB, using the MCUboot recovery mode."""

import asyncio
import json
import sys
import zipfile

from serial.tools import list_ports
from smp.error import MGMT_ERR
from smp.os_management import BootMode
from smpclient import SMPClient
from smpclient.generics import error_v1, success
from smpclient.requests.os_management import ResetWrite
from smpclient.transport.serial import BufferParams, SMPSerialTransport

USB_VID = 0x2FE3
APP_PID = 0xA0D1
BOOTLOADER_PID = 0xA0D2
# CONFIG_BOOT_MAX_LINE_INPUT_LEN and CONFIG_BOOT_LINE_BUFS of MCUboot, a frame that needs more
# lines than that is dropped
BOOTLOADER_LINE_LENGTH = 128
BOOTLOADER_LINE_BUFFERS = 8
PORT_TIMEOUT_S = 10
# The network core copies its image after the upload and refuses a reset until it is done
RESET_TRIES = 30


def find_port(pid: int) -> str | None:
    for port in list_ports.comports():
        if port.vid == USB_VID and port.pid == pid:
            return port.device
    return None


async def wait_for_port(pid: int) -> str:
    for _ in range(PORT_TIMEOUT_S * 10):
        port = find_port(pid)
        if port is not None:
            return port
        await asyncio.sleep(0.1)
    sys.exit("The bootloader did not show up on USB")


async def enter_bootloader(port: str) -> None:
    async with SMPClient(SMPSerialTransport(), port) as client:
        # ty does not understand the generic request types of smpclient
        request = ResetWrite(boot_mode=BootMode.BOOTLOADER)
        response = await client.request(request)  # ty: ignore[invalid-argument-type]
    if not success(response):
        sys.exit(f"Restart into the bootloader failed: {response}")


async def upload(client: SMPClient, name: str, slot: int, data: bytes) -> None:
    async for offset in client.upload(data, slot=slot):
        print(f"\r{name}: {offset * 100 // len(data)}%", end="", flush=True)
    print()


async def reset(client: SMPClient) -> None:
    for _ in range(RESET_TRIES):
        response = await client.request(ResetWrite())  # ty: ignore[invalid-argument-type]
        # MCUboot answers {"rc": 0}, which smpclient reads as an error with rc EOK
        if success(response) or (error_v1(response) and response.rc == MGMT_ERR.EOK):
            return
        if not (error_v1(response) and response.rc == MGMT_ERR.EBUSY):
            sys.exit(f"Restart of the bootloader failed: {response}")
        await asyncio.sleep(1)
    sys.exit("The bootloader did not restart, unplug the board to restart it")


async def main(package: str) -> None:
    with zipfile.ZipFile(package) as zip_file:
        manifest = json.loads(zip_file.read("manifest.json"))
        # slot_index_primary matches the image numbers of MCUBOOT_SERIAL_DIRECT_IMAGE_UPLOAD
        images = [
            (f["file"], int(f["slot_index_primary"]), zip_file.read(f["file"]))
            for f in manifest["files"]
        ]

    port = find_port(BOOTLOADER_PID)
    if port is None:
        app_port = find_port(APP_PID)
        if app_port is None:
            sys.exit("No AudioLink found on USB")
        await enter_bootloader(app_port)
        port = await wait_for_port(BOOTLOADER_PID)

    transport = SMPSerialTransport(
        BufferParams(BOOTLOADER_LINE_LENGTH, BOOTLOADER_LINE_BUFFERS)
    )
    async with SMPClient(transport, port) as client:
        # Network core first: if its upload fails, the application is still intact
        for name, slot, data in sorted(
            images, key=lambda image: image[1], reverse=True
        ):
            await upload(client, name, slot, data)
        await reset(client)
    print("Done, the board restarts with the new firmware")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(f"usage: {sys.argv[0]} dfu_application.zip")
    asyncio.run(main(sys.argv[1]))
