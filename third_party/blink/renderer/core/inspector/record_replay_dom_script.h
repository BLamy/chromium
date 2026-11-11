// Copyright 2023 Record Replay Inc. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_INSPECTOR_RECORD_REPLAY_DOM_SCRIPT_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_INSPECTOR_RECORD_REPLAY_DOM_SCRIPT_H_

namespace blink {

// The script record_replay_dom.cc runs in a page for the commands about its
// DOM. Everything from the first function on is upstream's
// replay-assets/replay_command_handlers.js, unchanged: the parts that preview
// nodes and styles and answer DOM.* and CSS.*.
inline constexpr char kRecordReplayDomScript[] = R"RRDOM((function(env) {
// The commands about a page's DOM and styles, and the previews of its nodes,
// as upstream's replay_command_handlers.js has them. Runs in the page, so
// |window| is the page's. |env| has what is kept outside the page: the IDs a
// replay knows objects by, and what Blink's inspector agents answer.
const {
  registerPlainObject,
  getPlainObjectByRrpId,
  registerRrpPreview,
  log,
  layoutDom,
  fromJsIsBlinkObject,
  fromJsIsBlinkNodeObject,
  fromJsIsBlinkElementObject,
  fromJsIsBlinkCSSStyleDeclarationObject,
  fromJsGetBoxModel,
  fromJsGetMatchedStylesForElement,
  fromJsCssGetStylesheetByCpdId,
  fromJsCollectEventListeners,
  fromJsDomPerformSearch,
  fromJsGetDevicePixelRatio,
} = env;

const EmptyArray = Object.freeze([]);
const JSON_stringify = JSON.stringify;
const Array_push = Array.prototype.push;
const Object_toString = Object.prototype.toString;
const String_ = String;
const String_slice = String.prototype.slice;

function warning(kind, data) {
  log(`[Warning] ${kind} ${JSON_stringify(data)}`);
}

function assert(v, msg = "") {
  if (!v) {
    const m = `Assertion failed when handling command (${msg})`;
    log(`[RuntimeError] ${m} - ${Error().stack}`);
    throw new Error(m);
  }
}

const isBlinkObject = fromJsIsBlinkObject;

// Style sheets by the ID the inspector's CSS agent gives them.
const gRrpIdByCdpId = new Map();
function registerRrpCpdId(rrpId, cdpId) {
  gRrpIdByCdpId.set(cdpId, rrpId);
}

const gLastBoundingClientRectsByNodeRrpId = new Map();
const gCssRulesByNodeRrpId = new Map();

// The agents are asked about a node by the node itself.
function getBlinkNodeIdByRrpId(nodeRrpId) {
  return getPlainObjectByRrpId(nodeRrpId);
}

function Graphics_getDevicePixelRatio() {
  return { ratio: fromJsGetDevicePixelRatio() || 0 };
}

function isIterable(obj) {
  return typeof obj?.[Symbol.iterator] === 'function';
}
function isArrayLike(obj) {
  return obj != null && typeof obj.length === "number";
}

function describeValueShape(value) {
  let str;
  try {
    str = String_(value);
  } catch {
    str = "<String failed>";
  }
  if (str.length > 120) {
    str = String_slice.call(str, 0, 120) + "...";
  }
  let tag;
  try {
    tag = Object_toString.call(value);
  } catch {
    tag = "<toString failed>";
  }
  return `${str} (typeof=${typeof value}, ctor=${value?.constructor?.name}, tag=${tag}, length=${value?.length}, blink=${!!fromJsIsBlinkObject(value)})`;
}

function iterateArrayLike(obj, fn, label) {
  if (isIterable(obj)) {
    for (const item of obj) {
      fn(item);
    }
  } else if (isArrayLike(obj)) {
    warning("IterateArrayLikeNotIterable", {
      label,
      value: describeValueShape(obj),
    });
    for (let i = 0; i < obj.length; i++) {
      fn(obj[i]);
    }
  } else if (obj != null) {
    warning("IterateArrayLikeNotArrayLike", {
      label,
      value: describeValueShape(obj),
    });
  }
}
function previewBlinkNode(node) {
  let attributes, pseudoType;
  if (fromJsIsBlinkElementObject(node)) {
    attributes = [];
    iterateArrayLike(node.attributes, ({ name, value }) => {
      Array_push.call(attributes, { name, value });
    }, "Element.attributes");
    // TODO: We cannot access pseudo elements using the JS DOM API - https://linear.app/replay/issue/RUN-953/
    // pseudoType = node.localName;
  }

  let style;
  if (node.style) {
    style = registerPlainObject(node.style);
  }

  let parentNode;
  if (node.parentNode) {
    parentNode = registerPlainObject(node.parentNode);
  } else if (
    node.defaultView &&
    node.defaultView.parent != node.defaultView &&
    node.defaultView.parent?.document
  ) {
    /**
     * Nested documents use the parent element instead of null.
     *
     * TODO: will need more work here to support multi-CSP iframes
     *   (properly handle `iframe`s and the case where `node.defaultView.parent.document` is missing)
     *   Issue: https://linear.app/replay/issue/RUN-954/dom-feature-support-multi-cspcross-origin-iframes
     */
    const iframes = node.defaultView.parent.document.getElementsByTagName(
      "iframe"
    );
    let iframe;
    iterateArrayLike(iframes, (f) => {
      if (!iframe && f.contentDocument == node) {
        iframe = f;
      }
    }, "Document.getElementsByTagName(iframe)");
    if (iframe) {
      parentNode = registerPlainObject(iframe);
    }
  }

  let documentURL;
  if (node.nodeType == Node.DOCUMENT_NODE) {
    documentURL = node.URL;
  }

  const rv = {
    nodeType: node.nodeType,
    nodeName: node.nodeName,
    nodeValue: typeof node.nodeValue === "string" ? node.nodeValue : undefined,
    isConnected: node.isConnected,
    attributes,
    pseudoType,
    style,
    parentNode,
    documentURL,
  };
  

  let childNodes;
  if (node.nodeName == "IFRAME" && node.contentDocument) {
    // Treat an iframe's content document as one of its child nodes.
    childNodes = [registerPlainObject(node.contentDocument)];
  } else if (node.childNodes?.length) {
    childNodes = [];
    iterateArrayLike(node.childNodes, (n) => {
      Array_push.call(childNodes, registerPlainObject(n));
    }, "Node.childNodes");
  }

  if (childNodes) {
    rv.childNodes = childNodes;
  }

  return rv;
}

function previewBlinkStyle(style) {
  // NOTE: this is for inline styles, where there is no parentRule
  let parentRule = undefined;

  const properties = [];
  for (let i = 0; i < style.length; i++) {
    const name = style.item(i);
    const value = style.getPropertyValue(name);
    if (value) {
      const important = style.getPropertyPriority(name) == "important" ? true : undefined;
      Array_push.call(properties, { name, value, important });
    }
  }

  return {
    cssText: style.cssText,
    parentRule,
    properties
  };
}


/** ###########################################################################
 * {@link DOM_forceLayout}
 * ##########################################################################*/
function DOM_forceLayout() {
  layoutDom();
  return {};
}

/** ###########################################################################
 * {@link DOM_getDocument}
 * ##########################################################################*/
function DOM_getDocument() {
  const rrpId = registerPlainObject(window.document);

  return {
    data: {},
    document: rrpId
  };
}

/** ###########################################################################
 * {@link DOM_getAllBoundingClientRects}
 * ##########################################################################*/

function getLastBoundingClientRect(nodeRrpId) {
  return gLastBoundingClientRectsByNodeRrpId.get(nodeRrpId);
}

/**
 * @see https://static.replay.io/protocol/tot/DOM/#type-NodeBounds
 */
function DOM_getAllBoundingClientRects() {
  const cx = new StackingContext(window);
  cx.addChildren(window.document);

  const entries = cx.flatten();
  // Get elements in front-to-back order.
  entries.reverse();

  const elements = entries
    .map((elem, i) => {
      const id = registerPlainObject(elem.raw) || i;

      // Use the containing context of the element to find any
      // applicable transform for its offset.
      const transformMatrix =
        elem.containingContext.findAncestor(parent => !!parent.transformMatrix)
          ?.transformMatrix;

      // Offset the bounding client rect by the transform matrix
      // and containing iframe offset (if any).
      let { left, top, right, bottom } = shiftRect(
        elem.raw.getBoundingClientRect(),
        elem.offset,
        transformMatrix
      );
      if (left >= right || top >= bottom) {
        return null;
      }

      // Get all client rects.
      const clientRects = [];
      iterateArrayLike(elem.raw.getClientRects(), (r) => {
        const { left, top, right, bottom } =
          shiftRect(r, elem.offset, transformMatrix);
        Array_push.call(clientRects, [left, top, right, bottom]);
      }, "Element.getClientRects");

      const clipBounds =
        shiftRect(elem.clipBounds, elem.offset, transformMatrix);
      // ignore elements that are completely outside their clipBounds
      if (
        clipBounds.left > right ||
        clipBounds.top > bottom ||
        clipBounds.right < left ||
        clipBounds.bottom < top
      ) {
        return null;
      }
      // only return the clipBounds that actually affect this element
      if (clipBounds.left === undefined || clipBounds.left <= left) {
        delete clipBounds.left;
      }
      if (clipBounds.top === undefined || clipBounds.top <= top) {
        delete clipBounds.top;
      }
      if (clipBounds.right === undefined || clipBounds.right >= right) {
        delete clipBounds.right;
      }
      if (clipBounds.bottom === undefined || clipBounds.bottom >= bottom) {
        delete clipBounds.bottom;
      }

      const v = {
        node: id,
        rect: [left, top, right, bottom],
      };
      if (clientRects.length > 0) {
        v.rects = clientRects;
      }
      if (Object.keys(clipBounds).length > 0) {
        v.clipBounds = clipBounds;
      }
      if (elem.style?.getPropertyValue("visibility") === "hidden") {
        v.visibility = "hidden";
      }
      if (elem.style?.getPropertyValue("pointer-events") === "none") {
        v.pointerEvents = "none";
      }

      gLastBoundingClientRectsByNodeRrpId.set(id, v);

      return v;
    })
    .filter((v) => !!v);

  return { elements };
};

/** ###########################################################################
 * {@link DOM_getBoundingClientRect}
 * ##########################################################################*/

/**
 * @see https://static.replay.io/protocol/tot/DOM/#type-BoxModel
 */
function DOM_getBoundingClientRect({ node }) {
  if (!gLastBoundingClientRectsByNodeRrpId.size) {
    // compute all basic bounding client rect sizes
    DOM_getAllBoundingClientRects();
  }
  const rects = getNodeBoundingClientRects(node);
  const rect = rects[0];

  return { rect };
}

/** ###########################################################################
 * {@link DOM_getBoxModel}
 * ##########################################################################*/

function getNodeBoundingClientRects(nodeRrpId) {
  const rectInfo = getLastBoundingClientRect(nodeRrpId);
  return rectInfo?.rects ||
    (rectInfo?.rect ?
      [rectInfo.rect] :
      [[0, 0, 20, 20]] // random default rect
    );
}

/**
 * @see https://static.replay.io/protocol/tot/DOM/#type-BoxModel
 */
function DOM_getBoxModel({ node: nodeRrpId }) {
  const nodeObj = getPlainObjectByRrpId(nodeRrpId);

  const model = {
    node: nodeRrpId
  };

  if (fromJsIsBlinkElementObject(nodeObj)) {
    const nodeId = getBlinkNodeIdByRrpId(nodeRrpId);
    /**
     * @see https://chromedevtools.github.io/devtools-protocol/tot/DOM/#type-BoxModel
     */
    const cdpModel = fromJsGetBoxModel(nodeId);

    if (cdpModel) {
      const {
        content, padding, border, margin,
        // width, height, shapeOutside
      } = cdpModel;
      Object.assign(
        model,
        {
          content,
          padding,
          border,
          margin
        }
      );
    }
  }

  if (!model.content) {
    // The given node does not have a box model.
    // -> Produce a "correct" output to prevent triggering of a session command
    // failure.
    // We do this because this is not technically a failure state. It makes
    // sense for the client to want to get a box model of a node no matter if it
    // has one or not.
    model.content = [];
    model.padding = [];
    model.border = [];
    model.margin = [];
  }

  return { model };
}


/** ###########################################################################
 * {@link DOM_getEventListeners}
 * ##########################################################################*/

function DOM_getEventListeners({ node }) {
  const nodeObject = getPlainObjectByRrpId(node);
  assert(nodeObject);

  const listenerInfos = fromJsCollectEventListeners(nodeObject);

  if (nodeObject.nodeName && nodeObject.nodeName == "HTML") {
    // Add event listeners for the document and window as well.
    Array_push.call(listenerInfos,
      ...fromJsCollectEventListeners(nodeObject.parentNode)   // document
      // ...fromJsCollectEventListeners(nodeObject.ownerGlobal)  // window
    );
  }

  const listeners = [];
  for (const { type, handler, capture } of listenerInfos) {
    if (!handler) {
      continue;
    }
    Array_push.call(listeners, {
      node,
      handler: registerPlainObject(handler),
      type,
      capture,
    });
  }

  return { listeners, data: {} };
}

/** ###########################################################################
 * {@link DOM_querySelector}
 * ##########################################################################*/

function DOM_querySelector({ node, selector }) {
  const nodeObj = getPlainObjectByRrpId(node);

  const resultObj = nodeObj.querySelector(selector);
  if (!resultObj) {
    return { data: {} };
  }
  const result = registerPlainObject(resultObj);
  return { result, data: {} };
}

/** ###########################################################################
 * {@link DOM_performSearch}
 * ##########################################################################*/

function DOM_performSearch({ query }) {
  query = query.trim();
  const nodeObjects = fromJsDomPerformSearch(query);
  const nodeRrpIds = nodeObjects
    ?.map(registerPlainObject)
    || [];

  return { nodes: nodeRrpIds, data: {} };
}


/** ###########################################################################
 * {@link CSS_getComputedStyle}
 * ##########################################################################*/

function CSS_getComputedStyle({ node }) {
  const nodeObj = getPlainObjectByRrpId(node);

  const computedStyle = [];
  if (fromJsIsBlinkElementObject(nodeObj)) {
    // NOTE: tested successfully for same-CSP elements of different iframes
    const ownerGlobal = window;

    // TODO: add pseudoType support - https://linear.app/replay/issue/RUN-953

    let styleInfo;
    // const pseudoType = getPseudoType(node);
    // if (pseudoType) {
    //   styleInfo = ownerGlobal.getComputedStyle(
    //     nodeObj.parentNode,
    //     pseudoType
    //   );
    // }
    // else {
    styleInfo = ownerGlobal.getComputedStyle(nodeObj);
    for (let i = 0; i < styleInfo.length; i++) {
      Array_push.call(computedStyle, {
        name: styleInfo.item(i),
        value: styleInfo.getPropertyValue(styleInfo.item(i)),
      });
    }
  }
  return { computedStyle };
}



/** ###########################################################################
 * {@link CSS_getAppliedRules}
 * ##########################################################################*/

/**
 *
 * @see https://developer.mozilla.org/en-US/docs/Web/API/CSSRule
 * @see https://developer.mozilla.org/en-US/docs/Web/API/CSSStyleRule
 * @see https://chromedevtools.github.io/devtools-protocol/tot/CSS/#type-CSSRule
 * @see https://static.replay.io/protocol/tot/CSS/#type-Rule
 */
function registerCdpAsRrpCssRule(nodeObj, cdpRule) {
  // NOTE: type is deprecated -> don't care
  const type = 1;
  let {
    selectorList = {},
    styleSheetId: styleSheetCpdId,
    style: {
      cssText: styleCssText,
      range: styleRange,
      cssProperties
    } = {},
    range: ruleRange,
    origin
  } = cdpRule || {};


  let styleSheetRrpId;
  if (styleSheetCpdId) {
    styleSheetRrpId = gRrpIdByCdpId.get(styleSheetCpdId);
    if (!styleSheetRrpId) {
      const nativeSheet = fromJsCssGetStylesheetByCpdId(styleSheetCpdId);

      // NOTE: `isSystem` is part of RRP from `gecko`.
      //    -> Chromium has a more diversified `StyleSheetOrigin` enum for this,
      //      (that is only accessible on the rule level in CDP, for some reason)
      const isSystem = origin !== 'regular';
      const styleSheet = { isSystem };
      if (nativeSheet?.href) {
        styleSheet.href = nativeSheet.href;
      }

      const styleSheetPreview = {
        className: 'RRPStyleSheetPreview', // no pre-defined className
        preview: {
          overflow: true,
          styleSheet
        }
      };
      styleSheetRrpId = registerRrpPreview(styleSheetPreview, nativeSheet);
      registerRrpCpdId(styleSheetRrpId, styleSheetCpdId);
    }
  }


  // stylePreview

  const properties = (cssProperties || [])
    .filter(prop => !!prop.text) // ignore props without text presentation
    .map(prop => {
      const { name, value, important } = prop;
      return {
        name,
        value,
        important
      };
    });
  /**
   * hackfix: for some reason, `user-agent` (and possibly other) styles don't have `cssText`.
   *    So, for now, we cook up a simple css serialization algo here.
   *    Native chromium has a better solution of course.
   * @see https://source.chromium.org/chromium/chromium/src/+/main:third_party/blink/renderer/core/css/style_property_serializer.cc;l=251;drc=3decef66bc4c08b142a19db9628e9efe68973e64
   * @see https://source.chromium.org/chromium/chromium/src/+/main:third_party/blink/renderer/core/css/style_property_serializer.cc;l=204;drc=3decef66bc4c08b142a19db9628e9efe68973e64
   */
  if (!styleCssText) {
    styleCssText = '\n  ' + properties
      .map(({ name, value, important }) => {
        const suffix = important ? ' !important' : '';
        return `${name}: ${value}${suffix};`;
      })
      .join('\n  ');
  }
  const stylePreview = {
    className: 'CSS2Properties', // `gecko` naming convention
    preview: {
      overflow: true,
      style: {
        cssText: styleCssText,
        parentRule: 0, // filled in once we have it, below
        properties
      }
    }
  };
  const nativeStlyeDeclaration = null;
  const styleRrpId = registerRrpPreview(stylePreview, nativeStlyeDeclaration);


  // rulePreview

  const maybeStartLine = (ruleRange || styleRange)?.startLine;

  // Lines from CDB data are zero-based.
  const startLine = maybeStartLine != null ? maybeStartLine + 1 : maybeStartLine;
  const startColumn = (ruleRange || styleRange)?.startColumn;
  // see https://static.replay.io/protocol/tot/CSS/#type-OriginalStyleSheetLocation
  const originalLocation = undefined; // TODO
  const selectorText = selectorList?.text || '';

  /**
   * Based on `CSSStyleRule::cssText()`.
   * @see https://github.com/replayio/chromium/blob/052831f0220b79fe0c3343b49f6d2863ea6de05d/third_party/blink/renderer/core/css/css_style_rule.cc#L94
   */
  const ruleCssText = `${selectorText} {${styleCssText}}`;

  const rulePreview = {
    className: 'CSSRule',
    preview: {
      overflow: true,
      rule: {
        type,
        cssText: ruleCssText,
        parentStyleSheet: styleSheetRrpId,
        startLine,
        startColumn,
        originalLocation,
        selectorText,
        style: styleRrpId
      }
    }
  };

  // NOTE: we cannot currently lookup the native `CSSRule` object because
  //      InspectorCSSAgent::BuildObjectForRuleWithoutMedia does not
  //      store an id.
  // const nativeRule = lookupNativeCssRuleByCdpRule();
  const nativeRule = null;
  const ruleRrpId = registerRrpPreview(rulePreview, nativeRule);

  // set ruleRrpId
  stylePreview && (stylePreview.preview.style.parentRule = ruleRrpId);

  return ruleRrpId;
}


/**
 * NOTE1: RRP's `CSS.Rule` is based on how gecko does things.
 *    gecko has a utility function to produce the rules in one call.
 *    But in chromium, we have to query and convert the data in multiple steps.
 *
 *
 * @see https://chromedevtools.github.io/devtools-protocol/tot/CSS/#method-getMatchedStylesForNode
 *
 * @see https://linear.app/replay/issue/RUN-981/enhance-pausegetobjectpreview-css-previews
 * @see https://github.com/replayio/gecko-dev/blob/628cc55f22785f3a66a8c767cdc86f31feb9a050/layout/inspector/InspectorUtils.cpp#L155
 */
function convertCdpToRrpCssRules(nodeObj, cdpMatchedStyles) {
  const appliedRules = [];

  const {
    matchedRules = EmptyArray,
    pseudoIdMatches = EmptyArray
  } = cdpMatchedStyles;

  function addCdpRule(cdpRule, pseudoElement = undefined) {
    const rrpRuleId = registerCdpAsRrpCssRule(nodeObj, cdpRule);
    const appliedRule = {
      rule: rrpRuleId,
      pseudoElement
    };
    Array_push.call(appliedRules, appliedRule);
  }

  for (const cdpRule of matchedRules.reverse()) {
    addCdpRule(cdpRule.rule);
  }

  for (const pseudoMatch of pseudoIdMatches) {
    const {
      // see: https://chromedevtools.github.io/devtools-protocol/tot/DOM/#type-PseudoType
      pseudoType,
      // pseudoIdentifier,
      matches
    } = pseudoMatch;
    for (const match of matches.reverse()) {
      addCdpRule(match.rule, pseudoType);
    }
  }

  return appliedRules;
}

function CSS_getAppliedRules({ node: nodeRrpId }) {
  const nodeObj = getPlainObjectByRrpId(nodeRrpId);

  let rules = gCssRulesByNodeRrpId.get(nodeRrpId);
  const data = {};

  if (!rules && fromJsIsBlinkNodeObject(nodeObj)) {
    const nodeId = getBlinkNodeIdByRrpId(nodeRrpId);

    // NOTE: CDP CSS domain commands are not enabled, so we have to get the data indirectly.
    // const cdpMatchedStyles = sendCDPMessage('CSS.getMatchedStylesForNode', { nodeId });
    if (fromJsIsBlinkElementObject(nodeObj)) {
      const cdpMatchedStyles = fromJsGetMatchedStylesForElement(nodeId) || { };
      rules = convertCdpToRrpCssRules(nodeObj, cdpMatchedStyles);
    } else {
      rules = [];
    }
    gCssRulesByNodeRrpId.set(nodeRrpId, rules);
  } else if (!rules) {
    // The target is not a node.
    log(`[RuntimeWarning] CSS.getAppliedRules called with non-node: ${nodeRrpId} ${isBlinkObject(nodeObj)} ${typeof nodeObj}.`);
    rules = [];
  }

  return { rules, data };
}

/** ###########################################################################
 * StackingContext
 * ##########################################################################*/
// Mouse Targets Overview
//
// Mouse target data is used to figure out which element to highlight when the
// mouse is hovered/clicked on different parts of the screen when the element
// picker is used. To determine this, we need to know the bounding client rects
// of every element (easy) and the order in which different elements are stacked
// (not easy).
//
// To figure out the order in which elements are stacked, we reconstruct the
// stacking contexts on the page and the order in which elements are laid out
// within those stacking contexts, allowing us to assemble a sorted array of
// elements such that for any two elements that overlap, the frontmost element
// appears first in the array.
//
// References:
//
// https://www.w3.org/TR/CSS21/zindex.html
//
//   We try to follow this reference, although not all of its rules are
//   implemented yet.
//
// https://developer.mozilla.org/en-US/docs/Web/CSS/CSS_Positioning/Understanding_z_index/The_stacking_context
//
//   This is helpful but the rules for when stacking contexts are created are
//   quite baroque and don't seem to match up with the spec above, so they are
//   mostly ignored here.

// Information about an element needed to add it to a stacking context.
function StackingContextElement(
  containingContext,
  node,
  parent,
  offset,
  style,
  clipBounds
) {
  // The stacking context this element is contained within.
  this.containingContext = containingContext;

  // Underlying element.
  this.raw = node;

  // Offset relative to the outer window of the window containing this context.
  this.offset = offset;

  // the parent StackingContextElement
  this.parent = parent;

  // Style and clipping information for the node.
  this.style = style;
  this.clipBounds = clipBounds;

  // Any stacking context at which this element is the root.
  this.context = null;
}

StackingContextElement.prototype = {
  isPositioned() {
    return this.style.getPropertyValue("position") != "static";
  },

  isAbsolutelyPositioned() {
    return ["absolute", "fixed"].includes(this.style.getPropertyValue("position"));
  },

  isTable() {
    return ["table", "inline-table"].includes(this.style.getPropertyValue("display"));
  },

  isFlexOrGridContainer() {
    return ["flex", "inline-flex", "grid", "inline-grid"].includes(
      this.style.getPropertyValue("display")
    );
  },

  isBlockElement() {
    return ["block", "table", "flex", "grid"].includes(this.style.getPropertyValue("display"));
  },

  isFloat() {
    return this.style.getPropertyValue("float") != "none";
  },

  getPositionedAncestor() {
    if (this.isPositioned()) {
      return this;
    }
    return this.parent?.getPositionedAncestor();
  },

  // see https://developer.mozilla.org/en-US/docs/Web/Guide/CSS/Block_formatting_context
  getFormattingContextElement() {
    if (!this.parent) {
      return this;
    }
    if (this.isFloat()) {
      return this;
    }
    if (this.isAbsolutelyPositioned()) {
      return this;
    }
    if (
      [
        "inline-block",
        "table-cell",
        "table-caption",
        "table",
        "table-row",
        "table-row-group",
        "table-header-group",
        "table-footer-group",
        "inline-table",
        "flow-root",
      ].includes(this.style.getPropertyValue("display"))
    ) {
      return this;
    }
    if (
      this.isBlockElement() &&
      !(
        ["visible", "clip"].includes(this.style.getPropertyValue("overflow-x")) &&
        ["visible", "clip"].includes(this.style.getPropertyValue("overflow-y"))
      )
    ) {
      return this;
    }
    if (["layout", "content", "paint"].includes(this.style.getPropertyValue("contain"))) {
      return this;
    }
    if (this.parent.isFlexOrGridContainer() && !this.isFlexOrGridContainer() && !this.isTable()) {
      return this;
    }
    if (
      this.style.getPropertyValue("column-count") != "auto" ||
      this.style.getPropertyValue("column-width") != "auto"
    ) {
      return this;
    }
    if (this.style.getPropertyValue("column-span") == "all") {
      return this;
    }
    return this.parent.getFormattingContextElement();
  },

  // toString() {
  //   return getObjectIdRaw(this.raw);
  // },
};

let gNextStackingContextId = 1;

// Information about all the nodes in the same stacking context.
// The spec says that some elements should be treated as if they
// "created a new stacking context, but any positioned descendants and
// descendants which actually create a new stacking context should be
// considered part of the parent stacking context, not this new one".
// For these elements we also create a StackingContext but pass the
// parent stacking context to the constructor as the "realStackingContext".
function StackingContext(window, options) {
  const {
    parentContext,
    root,
    offset,
    transformMatrix,
    realStackingContext
  } = options || {};
  this.window = window;
  this.parentContext = parentContext;
  this.id = gNextStackingContextId++;

  this.realStackingContext = realStackingContext || this;

  // Offset relative to the outer window of the window containing this context.
  this.offset = offset || { left: 0, top: 0 };

  // Transform scale parameter.  This is only relevant for stacking
  // contexts for IFRAME elements.
  if (transformMatrix) {
    assert(root && root.raw.tagName === "IFRAME");
  }
  this.transformMatrix = transformMatrix;

  // The arrays below are filled in tree order (preorder depth first traversal).

  // All non-positioned, non-floating elements.
  this.nonPositionedElements = [];

  // All floating elements.
  this.floatingElements = [];

  // All positioned elements with an auto or zero z-index.
  this.positionedElements = [];

  // Arrays of elements with non-zero z-indexes, indexed by that z-index.
  this.zIndexElements = new Map();

  this.root = root;
  if (root) {
    this.addChildrenWithParent(root);
  }
}

let gStackingContextWarn = 0;

StackingContext.prototype = {
  toString() {
    return `StackingContext:${this.id}`;
  },

  // Find the first parent stacking context matching the predicate.
  findAncestor(predicate) {
    let cur = this;
    while (cur) {
      if (predicate(cur)) {
        return cur;
      }
      cur = cur.parentContext;
    }
    return null;
  },

  // Add node and its descendants to this stacking context.
  add(node, parentElem, offset) {
    const style = this.window.getComputedStyle(node);
    if (!style) {
      // It's not 100% clear why this is sometimes null, but it seems like
      // this can happen if DOM commands are sent when the window is shutting
      // down in some way or another.
      return;
    }

    const position = style.getPropertyValue("position");
    let clipBounds;
    if (position == "absolute") {
      clipBounds = parentElem?.getPositionedAncestor()?.clipBounds || {};
    } else if (position == "fixed") {
      clipBounds = {};
    } else {
      clipBounds = parentElem?.clipBounds || {};
    }
    clipBounds = Object.assign({}, clipBounds);
    const cx = new StackingContextElement(this, node, parentElem, offset, style, clipBounds);
    if (!["HTML", "BODY"].includes(cx.raw.tagName)) {
      if (style.getPropertyValue("overflow-x") != "visible") {
        const clipBounds2 = cx.getFormattingContextElement().raw.getBoundingClientRect();
        cx.clipBounds.left =
          clipBounds.left !== undefined
            ? Math.max(clipBounds2.left, clipBounds.left)
            : clipBounds2.left;
        cx.clipBounds.right =
          clipBounds.right !== undefined
            ? Math.min(clipBounds2.right, clipBounds.right)
            : clipBounds2.right;
      }
      if (style.getPropertyValue("overflow-y") != "visible") {
        const clipBounds2 = cx.getFormattingContextElement().raw.getBoundingClientRect();
        cx.clipBounds.top =
          clipBounds.top !== undefined
            ? Math.max(clipBounds2.top, clipBounds.top)
            : clipBounds2.top;
        cx.clipBounds.bottom =
          clipBounds.bottom !== undefined
            ? Math.min(clipBounds2.bottom, clipBounds.bottom)
            : clipBounds2.bottom;
      }
    }

    // Create a new stacking context for any iframes.
    if (cx.raw.tagName == "IFRAME" && cx.raw.contentWindow?.document) {
      let { left, top } = cx.raw.getBoundingClientRect();

      // The left and top are adjusted by the transform matrix for
      // the containing iframe, if any.  For this, we just search up the
      // context chain, looking for one with a defined transform matrix.
      let parentTransformMatrix =
        this.findAncestor(parent => !!parent.transformMatrix)
          ?.transformMatrix;
      if (parentTransformMatrix) {
        const adjusted = adjustCoordinateByTransformMatrix(
          [ left, top ],
          parentTransformMatrix
        );
        left = adjusted[0];
        top = adjusted[1];
      }

      // Compute the transform matrix for the iframe within its containing
      // document.
      // If we have a parent transform matrix, multiply it with this one.
      let transformMatrix = computeTransformMatrix(cx.raw, this.window);
      if (parentTransformMatrix) {
        transformMatrix = multiplyTransformMatrix(
          parentTransformMatrix,
          transformMatrix
        );
      }

      this.addContext(cx, undefined, { left, top, transformMatrix });
      cx.context.addChildren(cx.raw.contentWindow.document);
    }

    if (!cx.style) {
      this.addNonPositionedElement(cx);
      this.addChildrenWithParent(cx);
      return;
    }

    const parentDisplay = cx.parent?.style?.getPropertyValue("display");
    if (
      position != "static" ||
      ["flex", "inline-flex", "grid", "inline-grid"].includes(parentDisplay)
    ) {
      const zIndex = cx.style.getPropertyValue("z-index");
      if (zIndex != "auto") {
        this.addContext(cx, undefined, {});
        // Elements with a zero z-index have their own stacking context but are
        // grouped with other positioned children with an auto z-index.
        const index = +zIndex | 0;
        if (index) {
          this.realStackingContext.addZIndexElement(cx, index);
          return;
        }
      }

      if (position != "static") {
        this.realStackingContext.addPositionedElement(cx);
        if (!cx.context) {
          this.addContext(cx, this.realStackingContext, {});
        }
      } else {
        this.addNonPositionedElement(cx);
        if (!cx.context) {
          this.addChildrenWithParent(cx);
        }
      }
      return;
    }

    if (cx.isFloat()) {
      // Group the element and its descendants.
      this.addContext(cx, this.realStackingContext, {});
      this.addFloatingElement(cx);
      return;
    }

    const display = cx.style.getPropertyValue("display");
    if (display == "inline-block" || display == "inline-table") {
      // Group the element and its descendants.
      this.addContext(cx, this.realStackingContext, {});
      this.addNonPositionedElement(cx);
      return;
    }

    // Handle opacity-based stacking context creation _after_
    // we check for positioned elements.
    // This is on the assumption that the rules for floating and
    // positioned elements should apply before the rules for opacity.

    // Elements with `opacity < 1` get their own stacking context.
    let opacity = 1;
    const opacityStr = cx.style.getPropertyValue("opacity");
    if (opacityStr !== undefined && opacityStr !== "") {
      opacity = +opacityStr;
    }
    if (opacity < 1) {
      this.addContext(cx, undefined, {});
    }

    this.addNonPositionedElement(cx);
    this.addChildrenWithParent(cx);
  },

  addContext(
    elem,
    realStackingContext,
    { left, top, transformMatrix } = {}
  ) {
    if (elem.context) {
      assert(!left && !top, "!left && !top");
      return;
    }

    left = left || 0;
    top = top || 0;

    const offset = {
      left: this.offset.left + left,
      top: this.offset.top + top,
    };
    elem.context = new StackingContext(this.window, {
      parentContext: this,
      root: elem,
      offset,
      realStackingContext,
      transformMatrix
    });
  },

  addZIndexElement(elem, index) {
    const existing = this.zIndexElements.get(index);
    if (existing) {
      Array_push.call(existing, elem);
    } else {
      this.zIndexElements.set(index, [elem]);
    }
  },

  addPositionedElement(elem) {
    Array_push.call(this.positionedElements, elem);
  },

  addFloatingElement(elem) {
    Array_push.call(this.floatingElements, elem);
  },

  addNonPositionedElement(elem) {
    Array_push.call(this.nonPositionedElements, elem);
  },

  addChildren(parentNode) {
    iterateArrayLike(parentNode.children, (child) => {
      if (!fromJsIsBlinkElementObject(child)) {
        return;
      }
      this.add(child, undefined, this.offset);
    }, "Element.children");
  },

  addChildrenWithParent(cx) {
    const children = cx.raw.children;
    if (!isIterable(children) && !isArrayLike(children)) {
      // [TT-253] `cx.raw` should always be an Element and
      // Element.prototype.children should always return an `HTMLCollection`.
      // Not sure why it sometimes complains about not being iterable.
      if (!fromJsIsBlinkElementObject(cx.raw) && !gStackingContextWarn) {
        ++gStackingContextWarn;
        warning("StackingContextNonElement", {
          value: describeValueShape(cx.raw),
          children: describeValueShape(children),
        });
      }
      return;
    }
    iterateArrayLike(children, (child) => {
      if (!fromJsIsBlinkElementObject(child)) {
        return;
      }
      this.add(child, cx, this.offset);
    }, "StackingContext Element.children");
  },

  // Get the elements in this context ordered back-to-front.
  flatten() {
    /**
     * @type {StackingContextElement[]}
     */
    const rv = [];

    const pushElements = (elems) => {
      for (const elem of elems) {
        if (elem.context && elem.context != this) {
          Array_push.call(rv, ...elem.context.flatten());
        } else {
          Array_push.call(rv, elem);
        }
      }
    };

    const pushZIndexElements = (filter) => {
      for (const z of zIndexes) {
        if (filter(z)) {
          pushElements(this.zIndexElements.get(z));
        }
      }
    };

    const zIndexes = [...this.zIndexElements.keys()];
    zIndexes.sort((a, b) => a - b);

    if (this.root) {
      pushElements([this.root]);
    }
    pushZIndexElements((z) => z < 0);
    pushElements(this.nonPositionedElements);
    pushElements(this.floatingElements);
    pushElements(this.positionedElements);
    pushZIndexElements((z) => z > 0);

    return rv;
  },
};

/** ###########################################################################
 * {@link shiftRect}
 * ##########################################################################*/
function shiftRect(rect, offset, transformMatrix) {
  // Apply the transform to the rect, before offsetting it.
  // The offset has already been adjusted as needed by any transforms
  // that apply to it.
  let { left, top, right, bottom } = rect;
  if (transformMatrix) {
    if (left && top) {
      const [ leftTrans, topTrans ] = adjustCoordinateByTransformMatrix(
        [ left, top ],
        transformMatrix
      );
      left = leftTrans;
      top = topTrans;
    }
    if (right && bottom) {
      const [ rightTrans, bottomTrans ] = adjustCoordinateByTransformMatrix(
        [ right, bottom ],
        transformMatrix
      );
      right = rightTrans;
      bottom = bottomTrans;
    }
  }

  left = rect.left !== undefined ?  offset.left + left : undefined;
  top = rect.top !== undefined ?  offset.top + top : undefined;
  right = rect.right !== undefined ?  offset.left + right : undefined;
  bottom = rect.bottom !== undefined ?  offset.top + bottom : undefined;
  return { left, top, right, bottom };
}

/** ###########################################################################
 * {@link parseCssTransformStringToMatrix}
 * Parses a CSS transform string into an 6-element array representing a 2D
 * transformation matrix.
 * ```
 * [ scaleX, skewX, skewY, scaleY, translateX, translateY ]
 * ```
 * On error, returns undefined.
 * ##########################################################################*/
function parseCssTransformStringToMatrix(transform) {
  if (!transform || transform === "none") {
    return;
  }
  try {
    // see https://developer.mozilla.org/en-US/docs/Web/API/CSSStyleValue/parse_static
    const parsedTransform = CSSStyleValue.parse(
      "transform",
      transform,
    );
    // FIXME: We only handle 2D transforms for now.
    if (!parsedTransform.is2D) {
      return;
    }
    if (parsedTransform.length > 0) {
      const { a, b, c, d, e, f } = parsedTransform[0].toMatrix();
      return [a, b, c, d, e, f];
    }
  } catch (err) {
    // FIXME: log a command diagnostic / warning.
  }
}

/** ###########################################################################
 * {@link computeTransformMatrix}
 * Compute the full transform for an element within its containing document.
 * ##########################################################################*/
function computeTransformMatrix(element, window) {
  let curMatrix = [1,0,0,1,0,0]; // start with identity matrix
  let curElem = element;
  while(curElem && fromJsIsBlinkElementObject(curElem)) {
    const transformStr = window.getComputedStyle(curElem).transform;
    const transformMatrix = parseCssTransformStringToMatrix(transformStr);
    if (transformMatrix) {
      curMatrix = multiplyTransformMatrix(transformMatrix, curMatrix);
    }
    curElem = curElem.parentNode;
  }
  return curMatrix;
}

/** ###########################################################################
 * {@link multiplyTransformMatrix}
 * Multiply two transform matrices of teh form [a, b, c, d, tx, ty]
 * ##########################################################################*/
function multiplyTransformMatrix(m1,m2) {
  // [a, b, c, d, tx, ty] => [
  //   a, c, tx,
  //   b, d, ty,
  //   0, 0, 1
  // ]
  const [a1, b1, c1, d1, tx1, ty1] = m1;
  const [a2, b2, c2, d2, tx2, ty2] = m2;

  const a3 = a1 * a2 + c1 * b2;
  const b3 = b1 * a2 + d1 * b2;
  const c3 = a1 * c2 + c1 * d2;
  const d3 = b1 * c2 + d1 * d2;
  const tx3 = a1 * tx2 + c1 * ty2 + tx1;
  const ty3 = b1 * tx2 + d1 * ty2 + ty1;
  return [a3, b3, c3, d3, tx3, ty3];
}

/** ###########################################################################
 * {@link adjustCoordinateByTransformMatrix}
 * Adjust a { left, top } coordinate by a transform matrix
 * ##########################################################################*/
function adjustCoordinateByTransformMatrix(coord, m) {
  const [ x, y ] = coord;
  const [scaleX, skewX, skewY, scaleY, translateX, translateY] = m;

  const x2 = x * scaleX + y * skewX + translateX;
  const y2 = x * skewY + y * scaleY + translateY;
  return [x2, y2];
}



return {
  commands: {
    "Graphics.getDevicePixelRatio": Graphics_getDevicePixelRatio,
    "DOM.forceLayout": DOM_forceLayout,
    "DOM.getDocument": DOM_getDocument,
    "DOM.getAllBoundingClientRects": DOM_getAllBoundingClientRects,
    "DOM.getBoundingClientRect": DOM_getBoundingClientRect,
    "DOM.getBoxModel": DOM_getBoxModel,
    "DOM.getEventListeners": DOM_getEventListeners,
    "DOM.querySelector": DOM_querySelector,
    "DOM.performSearch": DOM_performSearch,
    "CSS.getComputedStyle": CSS_getComputedStyle,
    "CSS.getAppliedRules": CSS_getAppliedRules,
  },
  previewBlinkNode,
  previewBlinkStyle,
};
})
)RRDOM";

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_INSPECTOR_RECORD_REPLAY_DOM_SCRIPT_H_
