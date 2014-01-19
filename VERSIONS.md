Versions
========

*Version 0.6.2*
- Minor fix to allow seamless compiling on fedora (cfitsio lib in different 
  location) and some optimizing on conditional compiling
- Fixed a bug in conditional code not allowing GTK2 clean compile
- Fixed some troubles when compiling on old distro
- Fixed a typo on message string context
- Introduced a basic user input filter for exp. time and temp.
- Found an fixed a bug with locale that use "," as decimal
- update Simple Chinese translation
- Fix TEC graph growing in time (again, sic)
- Temp reading feature for QHY5L-II
- QHY5L-II smoother operation, max_gain settings updated
- Solved CFW disconnect requiring 2 clicks in tty mode
- Update language file (Italian / French finalized) to let translators

*Version 0.7.0*
- Adding Avi save feature (8Bit data into RGB24 raw, registax compatible)
- Add split avi file > 2Gb, max counters to 9999
- Fixed crash when image save folder does not exist
- Changed how QHY5 reset is performed (much more dependable)
- Adding ROI feature for FWHM calculation. ROI will chase selected "star"
- Changed status bar position to top. Split in 3 for better space management
- Prevented strange behavior of sscanf with empty strings on some systems
- Reworked the "shutter trick" for QHY7
- Tec controlling loop is now based on a timer, proved more effective
- Fixed single core operation
- Better UI responsiveness during long waiting loops
- Speed up focus mode (fast download). Tec reading and feedback are disabled,
  restored when slow speed mode or capture is set
- New "dark / light" frame feature for QHY9

*Version 0.7.1*
- Add save image header info for fit files

*Version 0.7.2*
- Add save image header info for avi files (in a txt file)
- Image header info extended (camera pixel size x/y and observer)
- Vid/Pid of raw "Orion StarShooter" add to the list of QHY5 raw devices

*Version 0.7.3*
- New radio toggle-buttons for Capture/Focus mode selection
- Qhy8l now fully supported (special thanks to Anat Ruangrassamee for help)
