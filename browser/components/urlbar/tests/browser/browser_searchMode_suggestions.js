/* Any copyright is dedicated to the Public Domain.
   http://creativecommons.org/publicdomain/zero/1.0/ */

"use strict";

/**
 * Tests search suggestions in search mode.
 */

const DEFAULT_ENGINE_NAME = "Test";
const SUGGESTIONS_ENGINE_NAME = "searchSuggestionEngine.xml";
const MAX_HISTORICAL_SEARCH_SUGGESTIONS = UrlbarPrefs.get(
  "maxHistoricalSearchSuggestions"
);

let suggestionsEngine;
let defaultEngine;
let expectedFormHistoryResults = [];

add_task(async function setup() {
  suggestionsEngine = await SearchTestUtils.promiseNewSearchEngine(
    getRootDirectory(gTestPath) + SUGGESTIONS_ENGINE_NAME
  );

  let oldDefaultEngine = await Services.search.getDefault();
  defaultEngine = await Services.search.addEngineWithDetails(
    DEFAULT_ENGINE_NAME,
    {
      template: "http://example.com/?search={searchTerms}",
    }
  );
  await Services.search.setDefault(defaultEngine);
  await Services.search.moveEngine(suggestionsEngine, 0);

  async function cleanup() {
    await PlacesUtils.history.clear();
    await PlacesUtils.bookmarks.eraseEverything();
  }
  await cleanup();
  registerCleanupFunction(cleanup);

  // Add some form history for our test engine.  Add more than
  // maxHistoricalSearchSuggestions so we can verify that excess form history is
  // added after remote suggestions.
  for (let i = 0; i < MAX_HISTORICAL_SEARCH_SUGGESTIONS + 1; i++) {
    let value = `hello formHistory ${i}`;
    await UrlbarTestUtils.formHistory.add([
      { value, source: suggestionsEngine.name },
    ]);
    expectedFormHistoryResults.push({
      heuristic: false,
      type: UrlbarUtils.RESULT_TYPE.SEARCH,
      source: UrlbarUtils.RESULT_SOURCE.HISTORY,
      searchParams: {
        suggestion: value,
        engine: suggestionsEngine.name,
      },
    });
  }

  // Add other form history.
  await UrlbarTestUtils.formHistory.add([
    { value: "hello formHistory global" },
    { value: "hello formHistory other", source: "other engine" },
  ]);

  registerCleanupFunction(async () => {
    await Services.search.setDefault(oldDefaultEngine);
    await Services.search.removeEngine(defaultEngine);
    await UrlbarTestUtils.formHistory.clear();
  });

  await SpecialPowers.pushPrefEnv({
    set: [
      ["browser.search.separatePrivateDefault.ui.enabled", false],
      ["browser.urlbar.update2", true],
      ["browser.urlbar.update2.oneOffsRefresh", true],
    ],
  });
});

add_task(async function emptySearch() {
  await BrowserTestUtils.withNewTab("about:robots", async function(browser) {
    await SpecialPowers.pushPrefEnv({
      set: [["browser.urlbar.update2.emptySearchBehavior", 2]],
    });
    await UrlbarTestUtils.promiseAutocompleteResultPopup({
      window,
      value: "",
    });
    await UrlbarTestUtils.enterSearchMode(window);
    Assert.equal(gURLBar.value, "", "Urlbar value should be cleared.");
    // For the empty search case, we expect to get the form history relative to
    // the picked engine and no heuristic.
    await checkResults(expectedFormHistoryResults);
    await UrlbarTestUtils.exitSearchMode(window, { clickClose: true });
    await UrlbarTestUtils.promisePopupClose(window);
    await SpecialPowers.popPrefEnv();
  });
});

add_task(async function emptySearch_withHistory() {
  // URLs with the same host as the search engine.
  await PlacesTestUtils.addVisits([
    `http://mochi.test/`,
    // Should not be returned because it's a redirect source.
    `http://mochi.test/redirect`,
    {
      uri: `http://mochi.test/target`,
      transition: PlacesUtils.history.TRANSITIONS.REDIRECT_TEMPORARY,
      referrer: `http://mochi.test/redirect`,
    },
  ]);
  await BrowserTestUtils.withNewTab("about:robots", async function(browser) {
    await SpecialPowers.pushPrefEnv({
      set: [["browser.urlbar.update2.emptySearchBehavior", 2]],
    });
    await UrlbarTestUtils.promiseAutocompleteResultPopup({
      window,
      value: "",
    });
    await UrlbarTestUtils.enterSearchMode(window);
    Assert.equal(gURLBar.value, "", "Urlbar value should be cleared.");
    // For the empty search case, we expect to get the form history relative to
    // the picked engine, history without redirects, and no heuristic.
    await checkResults([
      ...expectedFormHistoryResults,
      {
        heuristic: false,
        type: UrlbarUtils.RESULT_TYPE.URL,
        source: UrlbarUtils.RESULT_SOURCE.HISTORY,
        url: `http://mochi.test/target`,
      },
      {
        heuristic: false,
        type: UrlbarUtils.RESULT_TYPE.URL,
        source: UrlbarUtils.RESULT_SOURCE.HISTORY,
        url: `http://mochi.test/`,
      },
    ]);

    await UrlbarTestUtils.exitSearchMode(window, { clickClose: true });
    await UrlbarTestUtils.promisePopupClose(window);
    await SpecialPowers.popPrefEnv();
  });

  await PlacesUtils.history.clear();
});

add_task(async function emptySearch_behavior() {
  // URLs with the same host as the search engine.
  await PlacesTestUtils.addVisits([`http://mochi.test/`]);

  await BrowserTestUtils.withNewTab("about:robots", async function(browser) {
    await SpecialPowers.pushPrefEnv({
      set: [["browser.urlbar.update2.emptySearchBehavior", 0]],
    });
    await UrlbarTestUtils.promiseAutocompleteResultPopup({
      window,
      value: "",
    });
    await UrlbarTestUtils.enterSearchMode(window);
    Assert.equal(gURLBar.value, "", "Urlbar value should be cleared.");
    // For the empty search case, we expect to get the form history relative to
    // the picked engine, history without redirects, and no heuristic.
    await checkResults([]);
    await UrlbarTestUtils.exitSearchMode(window, { clickClose: true });
    await UrlbarTestUtils.promisePopupClose(window);
    await SpecialPowers.popPrefEnv();
  });

  await BrowserTestUtils.withNewTab("about:robots", async function(browser) {
    await SpecialPowers.pushPrefEnv({
      set: [["browser.urlbar.update2.emptySearchBehavior", 1]],
    });
    await UrlbarTestUtils.promiseAutocompleteResultPopup({
      window,
      value: "",
    });
    await UrlbarTestUtils.enterSearchMode(window);
    Assert.equal(gURLBar.value, "", "Urlbar value should be cleared.");
    // For the empty search case, we expect to get the form history relative to
    // the picked engine, history without redirects, and no heuristic.
    await checkResults([...expectedFormHistoryResults]);
    await UrlbarTestUtils.exitSearchMode(window, { clickClose: true });
    await UrlbarTestUtils.promisePopupClose(window);
    await SpecialPowers.popPrefEnv();
  });

  await PlacesUtils.history.clear();
});

add_task(async function nonEmptySearch() {
  await BrowserTestUtils.withNewTab("about:robots", async function(browser) {
    let query = "hello";
    await UrlbarTestUtils.promiseAutocompleteResultPopup({
      window,
      value: query,
    });
    await UrlbarTestUtils.enterSearchMode(window);
    Assert.equal(gURLBar.value, query, "Urlbar value should be set.");
    // We expect to get the heuristic and all the suggestions.
    await checkResults([
      {
        heuristic: true,
        type: UrlbarUtils.RESULT_TYPE.SEARCH,
        source: UrlbarUtils.RESULT_SOURCE.SEARCH,
        searchParams: {
          query,
          engine: suggestionsEngine.name,
        },
      },
      ...expectedFormHistoryResults.slice(0, 2),
      {
        heuristic: false,
        type: UrlbarUtils.RESULT_TYPE.SEARCH,
        source: UrlbarUtils.RESULT_SOURCE.SEARCH,
        searchParams: {
          query,
          suggestion: `${query}foo`,
          engine: suggestionsEngine.name,
        },
      },
      {
        heuristic: false,
        type: UrlbarUtils.RESULT_TYPE.SEARCH,
        source: UrlbarUtils.RESULT_SOURCE.SEARCH,
        searchParams: {
          query,
          suggestion: `${query}bar`,
          engine: suggestionsEngine.name,
        },
      },
      ...expectedFormHistoryResults.slice(2, 4),
    ]);

    await UrlbarTestUtils.exitSearchMode(window, { clickClose: true });
    await UrlbarTestUtils.promisePopupClose(window);
  });
});

add_task(async function nonEmptySearch_nonMatching() {
  await BrowserTestUtils.withNewTab("about:robots", async function(browser) {
    let query = "ciao";
    await UrlbarTestUtils.promiseAutocompleteResultPopup({
      window,
      value: query,
    });
    await UrlbarTestUtils.enterSearchMode(window);
    Assert.equal(gURLBar.value, query, "Urlbar value should be set.");
    // We expect to get the heuristic and the remote suggestions since the local
    // ones don't match.
    await checkResults([
      {
        heuristic: true,
        type: UrlbarUtils.RESULT_TYPE.SEARCH,
        source: UrlbarUtils.RESULT_SOURCE.SEARCH,
        searchParams: {
          query,
          engine: suggestionsEngine.name,
        },
      },
      {
        heuristic: false,
        type: UrlbarUtils.RESULT_TYPE.SEARCH,
        source: UrlbarUtils.RESULT_SOURCE.SEARCH,
        searchParams: {
          query,
          suggestion: `${query}foo`,
          engine: suggestionsEngine.name,
        },
      },
      {
        heuristic: false,
        type: UrlbarUtils.RESULT_TYPE.SEARCH,
        source: UrlbarUtils.RESULT_SOURCE.SEARCH,
        searchParams: {
          query,
          suggestion: `${query}bar`,
          engine: suggestionsEngine.name,
        },
      },
    ]);

    await UrlbarTestUtils.exitSearchMode(window, { clickClose: true });
    await UrlbarTestUtils.promisePopupClose(window);
  });
});

add_task(async function nonEmptySearch_withHistory() {
  // URLs with the same host as the search engine.
  let query = "ciao";
  await PlacesTestUtils.addVisits([
    `http://mochi.test/${query}`,
    `http://mochi.test/${query}1`,
    // Should not be returned because it has a different host, even if it
    // matches the host in the path.
    `http://example.com/mochi.test/${query}`,
  ]);

  await BrowserTestUtils.withNewTab("about:robots", async function(browser) {
    await UrlbarTestUtils.promiseAutocompleteResultPopup({
      window,
      value: query,
    });

    await UrlbarTestUtils.enterSearchMode(window);
    Assert.equal(gURLBar.value, query, "Urlbar value should be set.");
    await checkResults([
      {
        heuristic: true,
        type: UrlbarUtils.RESULT_TYPE.SEARCH,
        source: UrlbarUtils.RESULT_SOURCE.SEARCH,
        searchParams: {
          query,
          engine: suggestionsEngine.name,
        },
      },
      {
        heuristic: false,
        type: UrlbarUtils.RESULT_TYPE.SEARCH,
        source: UrlbarUtils.RESULT_SOURCE.SEARCH,
        searchParams: {
          query,
          suggestion: `${query}foo`,
          engine: suggestionsEngine.name,
        },
      },
      {
        heuristic: false,
        type: UrlbarUtils.RESULT_TYPE.SEARCH,
        source: UrlbarUtils.RESULT_SOURCE.SEARCH,
        searchParams: {
          query,
          suggestion: `${query}bar`,
          engine: suggestionsEngine.name,
        },
      },
      {
        heuristic: false,
        type: UrlbarUtils.RESULT_TYPE.URL,
        source: UrlbarUtils.RESULT_SOURCE.HISTORY,
        url: `http://mochi.test/${query}1`,
      },
      {
        heuristic: false,
        type: UrlbarUtils.RESULT_TYPE.URL,
        source: UrlbarUtils.RESULT_SOURCE.HISTORY,
        url: `http://mochi.test/${query}`,
      },
    ]);

    await UrlbarTestUtils.exitSearchMode(window, { clickClose: true });
    await UrlbarTestUtils.promisePopupClose(window);
  });

  await PlacesUtils.history.clear();
});

add_task(async function nonEmptySearch_url() {
  await BrowserTestUtils.withNewTab("about:robots", async function(browser) {
    let query = "http://www.example.com/";
    await UrlbarTestUtils.promiseAutocompleteResultPopup({
      window,
      value: query,
    });
    await UrlbarTestUtils.enterSearchMode(window);

    // The heuristic result for a search that's a valid URL should be a search
    // result, not a URL result.
    await checkResults([
      {
        heuristic: true,
        type: UrlbarUtils.RESULT_TYPE.SEARCH,
        source: UrlbarUtils.RESULT_SOURCE.SEARCH,
        searchParams: {
          query,
          engine: suggestionsEngine.name,
        },
      },
    ]);

    await UrlbarTestUtils.exitSearchMode(window, { clickClose: true });
    await UrlbarTestUtils.promisePopupClose(window);
  });
});

async function checkResults(expectedResults) {
  Assert.equal(
    UrlbarTestUtils.getResultCount(window),
    expectedResults.length,
    "Check results count."
  );
  for (let i = 0; i < expectedResults.length; ++i) {
    info(`Checking result at index ${i}`);
    let expected = expectedResults[i];
    let actual = await UrlbarTestUtils.getDetailsOfResultAt(window, i);

    // Check each property defined in the expected result against the property
    // in the actual result.
    for (let key of Object.keys(expected)) {
      // For searchParams, remove undefined properties in the actual result so
      // that the expected result doesn't need to include them.
      if (key == "searchParams") {
        let actualSearchParams = actual.searchParams;
        for (let spKey of Object.keys(actualSearchParams)) {
          if (actualSearchParams[spKey] === undefined) {
            delete actualSearchParams[spKey];
          }
        }
      }
      Assert.deepEqual(
        actual[key],
        expected[key],
        `${key} should match at result index ${i}.`
      );
    }
  }
}
