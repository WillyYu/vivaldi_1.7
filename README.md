# vivaldi_1.7

## Skip console log for Vivaldi Render Process [2017-04-15]

### Detail
Each WebView's console log message will sent to the host.
Some website has lots message (debug/Warning/Error) during loading.
Vivaldi didn't listen WebView's console message event, so that can skip it to reduce the loading of Vivaldi's CrRenderMain


## Enhance the performance of tab resize when toggling Bookmark/Download/... panels.

### Detail
I usually hold more than 50 tabs open, it becomes slow to resize tabs when toggling bookmark panel.
More tabs were held, reisze will take more longer.

To toggle panel Vivaldi will trigger tab resize by my analysis.
But Chromium needs to set size to each WebView (tabs), and then handle the resize callback.
Like below image, open 47 tabs Chromium took around 202 ms to finish the layout on my computer(Intel i7).

<img src="https://github.com/WillyYu/vivaldi_1.7/blob/master/image/1_toggle_panel.png?raw=true"/>

So, I made some tricky change:

1. guest_view_container.js
    - Insert the attributes of webview to interanl element, so that can be identified

2. browser_plugin.cc
    - When resize event is fired, don't report geometry change. If:
        - Check the element whether Vivaldi's tab. (by attributes from geust_view_container.js)
        - Not visible

## Enhance the Bookmark panel's scrolling smoothness

### Detail
If there are many bookmarks on bookmark panel, the scrolling will be jank.
It seems the default props of React framework, the pageSize of List is 10 by default. 
If the item size exceeds the pageSize, then React need to re-layout the list componenet.

Like upper trace of below image, it takes around 26ms to layout the list content.
If the page size is changed to 200, the layout time will be decreased.
The result shown as bottom trace of below image:

<img src="https://github.com/WillyYu/vivaldi_1.7/blob/master/image/2_smoothness.png?raw=true"/>

Change:
vendor-bundle.js

This change just a simple hint, I think should be a configurable props on Bookmark modules of bundle.js

