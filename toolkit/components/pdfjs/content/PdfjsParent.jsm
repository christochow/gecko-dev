/* Copyright 2012 Mozilla Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

"use strict";

var EXPORTED_SYMBOLS = ["PdfjsParent"];

const { XPCOMUtils } = ChromeUtils.import(
  "resource://gre/modules/XPCOMUtils.jsm"
);

const { Services } = ChromeUtils.import("resource://gre/modules/Services.jsm");

ChromeUtils.defineModuleGetter(
  this,
  "SetClipboardSearchString",
  "resource://gre/modules/Finder.jsm"
);

ChromeUtils.defineModuleGetter(
  this,
  "PrivateBrowsingUtils",
  "resource://gre/modules/PrivateBrowsingUtils.jsm"
);

var Svc = {};
XPCOMUtils.defineLazyServiceGetter(
  Svc,
  "mime",
  "@mozilla.org/mime;1",
  "nsIMIMEService"
);

XPCOMUtils.defineLazyPreferenceGetter(
  this,
  "matchesCountLimit",
  "accessibility.typeaheadfind.matchesCountLimit"
);

let gFindTypes = [
  "find",
  "findagain",
  "findhighlightallchange",
  "findcasesensitivitychange",
  "findbarclose",
];

class PdfjsParent extends JSWindowActorParent {
  constructor() {
    super();
    this._boundToFindbar = null;
    this._findFailedString = null;
  }

  didDestroy() {
    if (this._boundToFindbar) {
      this._removeEventListener();
    }
  }

  receiveMessage(aMsg) {
    switch (aMsg.name) {
      case "PDFJS:Parent:displayWarning":
        this._displayWarning(aMsg);
        break;

      case "PDFJS:Parent:updateControlState":
        return this._updateControlState(aMsg);
      case "PDFJS:Parent:updateMatchesCount":
        return this._updateMatchesCount(aMsg);
      case "PDFJS:Parent:addEventListener":
        return this._addEventListener();
      case "PDFJS:Parent:saveURL":
        return this._saveURL(aMsg);
    }
    return undefined;
  }

  /*
   * Internal
   */

  get browser() {
    return this.browsingContext.top.embedderElement;
  }

  _saveURL(aMsg) {
    const data = aMsg.data;
    this.browser.ownerGlobal.saveURL(
      data.blobUrl /* aURL */,
      data.filename /* aFileName */,
      null /* aFilePickerTitleKey */,
      true /* aShouldBypassCache */,
      false /* aSkipPrompt */,
      null /* aReferrerInfo */,
      null /* aSourceDocument */,
      PrivateBrowsingUtils.isBrowserPrivate(
        this.browser
      ) /* aIsContentWindowPrivate */,
      Services.scriptSecurityManager.getSystemPrincipal() /* aPrincipal */
    );
  }

  _updateControlState(aMsg) {
    let data = aMsg.data;
    let browser = this.browser;
    let tabbrowser = browser.getTabBrowser();
    let tab = tabbrowser.getTabForBrowser(browser);
    tabbrowser.getFindBar(tab).then(fb => {
      if (!fb) {
        // The tab or window closed.
        return;
      }
      fb.updateControlState(data.result, data.findPrevious);

      if (
        data.result === Ci.nsITypeAheadFind.FIND_FOUND ||
        data.result === Ci.nsITypeAheadFind.FIND_WRAPPED ||
        (data.result === Ci.nsITypeAheadFind.FIND_PENDING &&
          !this._findFailedString)
      ) {
        this._findFailedString = null;
        SetClipboardSearchString(data.rawQuery);
      } else if (!this._findFailedString) {
        this._findFailedString = data.rawQuery;
        SetClipboardSearchString(data.rawQuery);
      }

      const matchesCount = this._requestMatchesCount(data.matchesCount);
      fb.onMatchesCountResult(matchesCount);
    });
  }

  _updateMatchesCount(aMsg) {
    let data = aMsg.data;
    let browser = this.browser;
    let tabbrowser = browser.getTabBrowser();
    let tab = tabbrowser.getTabForBrowser(browser);
    tabbrowser.getFindBar(tab).then(fb => {
      if (!fb) {
        // The tab or window closed.
        return;
      }
      const matchesCount = this._requestMatchesCount(data);
      fb.onMatchesCountResult(matchesCount);
    });
  }

  _requestMatchesCount(data) {
    if (!data) {
      return { current: 0, total: 0 };
    }
    let result = {
      current: data.current,
      total: data.total,
      limit: typeof matchesCountLimit === "number" ? matchesCountLimit : 0,
    };
    if (result.total > result.limit) {
      result.total = -1;
    }
    return result;
  }

  handleEvent(aEvent) {
    const type = aEvent.type;
    // Handle the tab find initialized event specially:
    if (type == "TabFindInitialized") {
      let browser = aEvent.target.linkedBrowser;
      this._hookupEventListeners(browser);
      aEvent.target.removeEventListener(type, this);
      return;
    }

    // To avoid forwarding the message as a CPOW, create a structured cloneable
    // version of the event for both performance, and ease of usage, reasons.
    let detail = null;
    if (type !== "findbarclose") {
      detail = {
        query: aEvent.detail.query,
        caseSensitive: aEvent.detail.caseSensitive,
        entireWord: aEvent.detail.entireWord,
        highlightAll: aEvent.detail.highlightAll,
        findPrevious: aEvent.detail.findPrevious,
      };
    }

    let browser = aEvent.currentTarget.browser;
    if (!this._boundToFindbar) {
      throw new Error(
        "FindEventManager was not bound for the current browser."
      );
    }
    browser.sendMessageToActor(
      "PDFJS:Child:handleEvent",
      { type, detail },
      "Pdfjs"
    );
    aEvent.preventDefault();
  }

  _addEventListener() {
    let browser = this.browser;
    if (this._boundToFindbar) {
      throw new Error(
        "FindEventManager was bound 2nd time without unbinding it first."
      );
    }

    this._hookupEventListeners(browser);
  }

  /**
   * Either hook up all the find event listeners if a findbar exists,
   * or listen for a find bar being created and hook up event listeners
   * when it does get created.
   */
  _hookupEventListeners(aBrowser) {
    let tabbrowser = aBrowser.getTabBrowser();
    let tab = tabbrowser.getTabForBrowser(aBrowser);
    let findbar = tabbrowser.getCachedFindBar(tab);
    if (findbar) {
      // And we need to start listening to find events.
      for (var i = 0; i < gFindTypes.length; i++) {
        var type = gFindTypes[i];
        findbar.addEventListener(type, this, true);
      }
      this._boundToFindbar = findbar;
    } else {
      tab.addEventListener("TabFindInitialized", this);
    }
    return !!findbar;
  }

  _removeEventListener() {
    // make sure the listener has been removed.
    let findbar = this._boundToFindbar;
    if (findbar) {
      // No reason to listen to find events any longer.
      for (var i = 0; i < gFindTypes.length; i++) {
        var type = gFindTypes[i];
        findbar.removeEventListener(type, this, true);
      }
    }

    this._boundToFindbar = null;
  }

  /*
   * Display a notification warning when the renderer isn't sure
   * a pdf displayed correctly.
   */
  _displayWarning(aMsg) {
    let data = aMsg.data;
    let browser = this.browser;

    let tabbrowser = browser.getTabBrowser();
    let notificationBox = tabbrowser.getNotificationBox(browser);

    // Flag so we don't send the message twice, since if the user clicks
    // "open with different viewer" both the button callback and
    // eventCallback will be called.
    let messageSent = false;
    let sendMessage = download => {
      this.sendAsyncMessage("PDFJS:Child:fallbackDownload", { download });
    };
    let buttons = [
      {
        label: data.label,
        accessKey: data.accessKey,
        callback() {
          messageSent = true;
          sendMessage(true);
        },
      },
    ];
    notificationBox.appendNotification(
      data.message,
      "pdfjs-fallback",
      null,
      notificationBox.PRIORITY_INFO_MEDIUM,
      buttons,
      function eventsCallback(eventType) {
        // Currently there is only one event "removed" but if there are any other
        // added in the future we still only care about removed at the moment.
        if (eventType !== "removed") {
          return;
        }
        // Don't send a response again if we already responded when the button was
        // clicked.
        if (messageSent) {
          return;
        }
        sendMessage(false);
      }
    );
  }
}
