sdk_generate_library()
sdk_library_add_sources(usb_audio.c usb_video.c)
sdk_library_add_sources(usb_hc_ehci_iso.c)
# sdk_add_include_directories(.)

sdk_add_include_directories(../cherryusb/port/ehci)

sdk_add_compile_definitions(-DCONFIG_USB_EHCI_ISO)
