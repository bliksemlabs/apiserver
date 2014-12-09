apiserver
=========

Current status: experimental sketch-pad, work in progress. 

For the moment, this repo exists for testing various IO and scaling approaches, rather than as a production component.

Much of this functionality can be provided by existing stable http libraries such as microhttpd but there are a few special cases on the margins, such as streaming back very large responses to the HTTP client without copying between multiple buffers.
