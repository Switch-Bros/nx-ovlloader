# nx-ovlloader
Host process for loading Switch overlay OVLs (NROs)

This is the loader service of the Ultrahand Overlay / Tesla ecosystem. It's derrived from the default nx-hbloader.
When being run, this service automatically tries to chainload `/switch/.overlays/ovlmenu.ovl`, Ultrahand Overlay or Tesla Menu. From there on it will load and switch between different overlays on request. 
