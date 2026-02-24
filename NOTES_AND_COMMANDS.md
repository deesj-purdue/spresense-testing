# COMMANDS
## Configure custom apps (Linux/WSL)
`tools/config.py default examples/usb_image examples/usb_camera`

## Build (Linux/WSL)
`make -j$(nproc)`

## Flash (Windows)
`./flash_writer.exe -c COM6 -d -n -e nuttx nuttx.spk`
- Note COM6 is the main board port, replace with your port

## Read NSH shell (Windows)
`python .\serialshell.py`
- For random noise image testing, send `usb_image [noise %]`
- For reading camera and sending over USB CDC, send `usb_camera`

## Read images being sent over USB CDC (Windows)
`python .\usb_image_receiver.py COM12`
- Note COM6 is the extension board port, replace with your port

# NOTES
- The *main board* is the small one, the *extension board* is the large one
- This uses USB CDC to send data over the serial com port
- The baudrate shouldn't matter as it should run at USB 2.0 speeds
- Certain commands work only/better on Linux/WSL or Windows, these are marked in the heading
- It is best to do all `git` commands in Linux/WSL to avoid CRLF issues that spresense expects to be Linux format
- The CRLF issues can also be caused when writing new code from a Windows machine (i think)