# AqMD3 Acquisition Console

An application for controlling and acquiring data from an Acqiris SA220P digitizer card intended for use with Ion-Mobility Spectrometry systems.

## About this fork

This is the University of Washington Bush lab's fork of
[PNNL-Comp-Mass-Spec/AqMD3-Acquisition-Console](https://github.com/PNNL-Comp-Mass-Spec/AqMD3-Acquisition-Console),
branched from upstream commit `1b8c964`. The original is Copyright Battelle Memorial Institute
and is BSD 2-Clause; `license.txt` and `disclaimer.txt` are unchanged and apply to this fork too.
We are grateful for the work, and for its being published.

The fork exists so that settings the upstream application compiles in can be set in `config.txt`
instead. It changes nothing about the ZeroMQ protocol, in either direction, so a client works
against either build without knowing which one answered.

- **Six settings move from source into `config.txt`**: `TriggerLevel`, `TriggerSlope`,
  `FullScaleRange`, `ZeroSuppressThreshold`, `ZeroSuppressHysteresis` and `ControlIoPort`. Each
  default is the literal upstream carries, so a config file naming none of them behaves exactly
  as the upstream application does. A value the application can name as wrong is refused at
  startup, naming the key, rather than being handed to the driver.
- **`info` names the fork**, appending `Fork: <repository>@<branch>` to the version string.
- **The log is flushed** as it is written. The application has no quit command, so it is always
  ended by being killed, and spdlog flushes only when its sinks are destroyed; without this the
  log file is empty after every shutdown.
- **Two string bugs in `SA220::get_digitizer_info` are fixed.** The model came back padded to 256
  bytes with NUL, and the serial buffer was resized to the length of the model string, so a
  ten-character serial number arrived truncated to six.
- **libzmq is built without its AF_UNIX signaler**, through the overlay triplet in
  `vcpkg-triplets/`, which `build.ps1` selects. On at least one Windows 11 machine, libzmq's
  AF_UNIX socket pair binds and then fails to connect under `%LOCALAPPDATA%\Temp`; because
  `make_fdpair` stops considering the TCP fallback once the bind succeeds, the signaler ends up
  holding invalid descriptors and the process aborts on the first `epoll_ctl`. Turning
  `ZMQ_HAVE_IPC` off gives the TCP loopback signaler instead. This application uses only `tcp://`.

Build it with `build.ps1`, which selects the overlay triplet:

```powershell
PS> .uild.ps1 -Config Release -VcpkgToolchain "D:/vcpkg/scripts/buildsystems/vcpkg.cmake"
```


## Building

Microsoft Visual Studio 2019 version 16.9 or newer required.

Third-party dependencies are managed using vcpkg, the utility and installation instructions can be found [here](https://github.com/microsoft/vcpkg).

Once vcpkg is installed, navigate to the directoy containing vcpkg.exe and execute the following command to install required third-party dependencies:
```powershell
PS C:\vcpkg> .\vcpkg.exe install zeromq:x64-windows sqlitecpp:x64-windows sqlite3:x64-windows snappy:x64-windows protobuf:x64-windows picosha2:x64-windows cppzmq:x64-windows
```

The **CMAKE_TOOLCHAIN_FILE** variable must have its value updated to be the file path for vcpkg.cmake, an example of this is shown [here](https://github.com/microsoft/vcpkg/blob/master/docs/examples/installing-and-using-packages.md) under **Section 2: Use**.

Acqiris MD3 Software and drivers for the SA220P must be installed on the system to both run and build the application. The software installer is made available by Acqiris and can be found [here](https://extranet.acqiris.com/homepage?field_res_products_target_id=23).



## Contacts

Written by Cameron Giberson and Joon-Yong Lee for the Department of Energy (PNNL, Richland, WA)\
Copyright 2021, Battelle Memorial Institute. All Rights Reserved.\
E-mail: cameron.giberson@pnnl.gov or proteomics@pnnl.gov\
Website: https://omics.pnl.gov/ or https://panomics.pnnl.gov/


## License

AqMD3 Acquisition Console is licensed under the 2-Clause BSD License; you may not use this program 
except in compliance with the License. You may obtain a copy of the License at 
https://opensource.org/licenses/BSD-2-Clause

Copyright 2021 Battelle Memorial Institute