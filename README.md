# vivaldi_1.7

## Issue 1
* Toggle Bookmark/Download/... panel is slow when visiting many tabs

### Detail
When many tabs(WebView) were active at the same time, then toggle on/off the panels.
Chromium needs set size to each WebView, It may take for a while.
For My Computer(Intel i7), open 47 tabs Chromium took around 202 ms to finish the layout.

I usually keep more than 50 tabs open, it is so slow to open bookmark panel and link to another page.

So, I made some tricky change:

1. guest_view_container.js
    - insert the attributes of webview to interanl element
    
2. browser_plugin.cc
    - When resize event is fired, don't report geometry change. If:
        - check the element whether Vivaldi's tab. (by attributes from geust_view_container.js)
        - is not visible

## Issue 2
* Enhance the Bookmark panel's scrolling smoothness




