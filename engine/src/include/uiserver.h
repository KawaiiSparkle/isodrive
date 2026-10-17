#ifndef ISODRIVE_UISERVER_H
#define ISODRIVE_UISERVER_H

// Loopback HTTP server used by "isodrive ui start" on managers without a
// WebUI bridge (pure Magisk): serves the module webroot and exposes
// POST /api/exec -> runs "isodrive ui <args>" and returns JSON.
int isodrive_ui_server_main(int argc, char** argv);

#endif  // ISODRIVE_UISERVER_H
