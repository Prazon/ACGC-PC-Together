ACGC PC Port - Windows x86_64 dedicated-town build
==================================================

This package intentionally contains no Animal Crossing assets or saves.

Client setup
------------
1. Put your legally obtained USA Rev 0 disc image (.iso, .gcm, or .ciso) in rom\.
2. For offline play, leave network.ini disabled and run AnimalCrossing.exe.
3. For online play, edit network.ini: set enabled=true, point server at
   HOST:PORT, use a unique persistent account_id, and copy the host's private
   invite_key. Then launch AnimalCrossing.exe normally.
4. Command-line options are still available as temporary overrides.

Server setup
------------
1. Edit server.ini. Set the town name, optional starting date/time, time zone,
   UDP port, data directory, and a private invitation key.
2. Run start_server.cmd. The real-time dashboard shows all resident slots,
   visitors, town date/time/weather, world readiness, traffic/errors/jobs, and
   timestamped activity. It remains visible even when stdout is logged.
3. Allow the configured UDP port in the host firewall/router if remote players connect.

The server stores its journal, SQLite database, checkpoints, and GCI under
towns\default\. Back up that directory while the server is stopped, or use
AnimalCrossingServer.exe --data towns\default --checkpoint-now first.

Players in the same resident house share its furniture layout, switches,
lights, and music. Each exterior follows its synchronized interior light switch
with the original day/night presentation. Other players arrive at the exact
destination doorway and use the original house-door animation.

Never publish the invitation key, town data, GCI files, or disc image.
