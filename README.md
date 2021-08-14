# XR Vessels
XR Series vessel add-ons for Orbiter Space Flight Simulator

## License

The XR vessels and associated utility projects are open source and licensed under the MIT license. Refer to the [LICENSE](./LICENSE) file for details.

## Installing and Flying the XR Vessels

Refer to the [XR Flight Operations Manual](./XRVessels/XR%20Flight%20Operations%20Manual.pdf) for details about how to install and fly the XR vessels in Orbiter.

## Building the XR Vessels 

You do not need to build the XR vessels in order to use them with Orbiter. However, if you want to build the XR vessels from the source, follow the steps below. These instructions assume you are building both the 32-bit and 64-bit versions. However, you may build only one version if you prefer.

1. Install Visual Studio 2019 from https://visualstudio.microsoft.com/downloads/.
3. Download and install (or build) Orbiter 2016 or later from either https://github.com/mschweiger/orbiter or http://orbit.medphys.ucl.ac.uk/download.html.
4. Clone the XRVessels repository from GitHub to your local machine with:
```bash
git clone git@github.com:dbeachy1/XRVessels.git
```
or
```bash
git clone https://github.com/dbeachy1/XRVessels.git
```

If you're looking for an excellent GUI that makes working with Git easier, I recommend [Tower](https://www.git-tower.com/).

5. Create four environment variables, either in your Windows environment settings or by adding them to `XRVessels\GlobalShared.props`.

* `ORBITER_ROOT` => your 32-bit Orbiter root folder
* `ORBITER_ROOT_X64` => your 64-bit Orbiter root folder
* `ORBITER_EXE` => `path\filename` relative to `ORBITER_ROOT` of the Orbiter executable to run when launching the debugger for 32-bit XR vessel builds; e.g., `orbiter.exe`
* `ORBITER_EXE_X64` => `path\filename` relative to `ORBITER_ROOT_x64` of the Orbiter executable to run when launching the debugger for 64-bit XR vessel builds; e.g., `Modules\Server\orbiter.exe`

6. Install 32-bit Orbiter to `%ORBITER_ROOT%`.
7. Install 64-bit Orbiter to `%ORBITER_ROOT_X64%`.
8. Download and install the latest XR vessels binary packages for all the vessels versions you want to build from `https://www.alteaaerospace.com`.

Now you are ready to compile and link the XR Vessels.

14. Bring up Visual Studio 2019 and open the solution `XRVessels\XRVessels.sln`.
15. Set the desired build target (e.g., `Debug x64`) and click `Build -> Rebuild Solution`; this will build all the XR vessel DLLs and copy both the DLLs and the `.cfg` files for each vessel to their proper locations under `%ORBITER_ROOT%` or `%ORBITER_ROOT_64` via Post-Build Events. If you get any build errors, double-check that the above environment variables are set correctly and that you restarted Visual Studio 2019 _after_ you defined those environment variables.
16. After the build succeeds, click `Debug -> Start Debugging` to bring up Orbiter under the Visual Studio debugger, then load your desired XR vessel scenario. You can now debug the XR vessels you just built.

For more information and support about Orbiter and the XR vessels, visit https://www.orbiter-forum.com/.

Happy Orbiting!
