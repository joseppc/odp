# Simple DDF example

This is a simple example that shows how to use the Device Driver Framework (DDF).

The devices in this case are files, and each file represents a different device,
and in turn each file type represents a differnt device type. This way files of
the same type can be used with the same device driver. Different versions of the
format of the file can be used with different versions of the DevIO interface.

## Proposed development plan

1. Blind programming the initial implementation of the example including:
    - Enumerator class
    - Enumerator (reads files in one directory)
    - Stub Device driver
         - Device driver for empty files (could return frames with zeroes)
    - Stub DevIO for files (how to simulate that?)
        - traditional read/write/seek?
        - mmaped DevIO operations?

2. Make the driver buildable as an example.

3. Implement proper drivers, proposed:
   - pcap driver (so we can save frames from other devices and replay later)
   - raw driver (save full frames, files could be mmaped as if it was a pool?)

## Notes

* When registering an enumerator, you need the enumerator's class handle.
  You can only get that if your enumerator is compiled together with the
  class. If that is correct, why do you need to go through all the
  complications of defining an api_name and version to match against?
