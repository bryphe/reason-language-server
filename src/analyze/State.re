open Infix;

type state = {
  rootPath: string,
  rootUri: string,
  compilerPath: string,
  refmtPath: string,
  documentText: Hashtbl.t(string, (string, int, bool)),
  documentTimers: Hashtbl.t(string, float),
  clientNeedsPlainText: bool,

  /* Might change based on bsconfig.json / .merlin */
  includeDirectories: list(string),
  compilationFlags: string,

  /* Depend on bsb having already run */
  localCompiledBase: string,
  localModules: list((string, (string, string))),
  localCompiledMap: list((string, string)),
  dependencyModules: list((FindFiles.modpath, (string, option(string)))),
  cmtCache:
    Hashtbl.t(
      string,
      (
        float, /* modified time */
        Cmt_format.cmt_infos,
        (option(string), list(Docs.full))
      )
    ),
  cmiCache:
    Hashtbl.t(
      string,
      (
        float, /* modified time */
        Cmi_format.cmi_infos,
        (option(string), list(Docs.full))
      )
    ),
  pathsForModule: Hashtbl.t(string, (string, option(string))),
  compiledDocuments: Hashtbl.t(string, AsYouType.result),
  lastDefinitions: Hashtbl.t(string, Definition.moduleData),

  /* workspace folders... */
};

module Show = {
  let _modPath = mp => switch mp {
    | FindFiles.Plain(s) => s
    | FindFiles.Namespaced(ns, name) => ns ++ "." ++ name
  };
  let state = ({rootPath, compilerPath, localModules, dependencyModules}) => {
    "Root: " ++ rootPath ++
    "\nLocal\n"++
    (Belt.List.map(localModules, ((name, (cmt, src))) => Printf.sprintf("%s (%s : %s)", name, cmt, src)) |> String.concat("\n"))
    ++
    "\nDeps\n" ++ 
    (Belt.List.map(dependencyModules, ((modpath, (cmt, src))) => Printf.sprintf("%s (%s : %s)", _modPath(modpath), cmt, src |? "")) |> String.concat("\n"))
  };
};

let isMl = path =>
  Filename.check_suffix(path, ".ml") || Filename.check_suffix(path, ".mli");

let odocToMd = text => MarkdownOfOCamldoc.convert(0, text);
let compose = (fn1, fn2, arg) => fn1(arg) |> fn2;

let converter = (src, usePlainText) => {
  let mlToOutput = compose(odocToMd, usePlainText ? Omd.to_text : Omd.to_markdown);
  fold(
    src,
    mlToOutput,
    src => isMl(src) ? mlToOutput : (usePlainText ? compose(Omd.of_string, Omd.to_text) : (x => x))
  );
};

let newDocsForCmt = (cmtCache, changed, cmt, src, clientNeedsPlainText) => {
    let infos = Cmt_format.read_cmt(cmt);
    switch (Docs.forCmt(converter(src, clientNeedsPlainText), infos)) {
    | None => {Log.log("Docs.forCmt gave me nothing " ++ cmt);None}
    | Some(docs) =>
      Hashtbl.replace(cmtCache, cmt, (changed, infos, docs));
      Some(docs);
    };
};

let newDocsForCmi = (cmiCache, changed, cmi, src, clientNeedsPlainText) => {
    let infos = Cmi_format.read_cmi(cmi);
    switch (Docs.forCmi(converter(src, clientNeedsPlainText), infos)) {
    | None => {Log.log("Docs.forCmi gave me nothing " ++ cmi);None}
    | Some(docs) =>
      Hashtbl.replace(cmiCache, cmi, (changed, infos, docs));
      Some(docs);
    };
};

let hasProcessedCmt = (state, cmt) => Hashtbl.mem(state.cmtCache, cmt);

let docsForCmt = (cmt, src, state) => {
  if (Filename.check_suffix(cmt, ".cmi")) {

  if (Hashtbl.mem(state.cmiCache, cmt)) {
    let (mtime, infos, docs) = Hashtbl.find(state.cmiCache, cmt);
    /* TODO I should really throttle this mtime checking to like every 50 ms or so */
    switch (Files.getMtime(cmt)) {
    | None => {Log.log("⚠️ cannot get docs for nonexistant cmt " ++ cmt); None}
    | Some(changed) =>
      if (changed > mtime) {
        newDocsForCmi(state.cmiCache, changed, cmt, src, state.clientNeedsPlainText);
      } else {
        Some(docs);
      }
    };
  } else {
    switch (Files.getMtime(cmt)) {
    | None => {Log.log("⚠️ cannot get docs for nonexistant cmt " ++ cmt); None}
    | Some(changed) => newDocsForCmi(state.cmiCache, changed, cmt, src, state.clientNeedsPlainText)
    };
  };


  } else {


  if (Hashtbl.mem(state.cmtCache, cmt)) {
    let (mtime, infos, docs) = Hashtbl.find(state.cmtCache, cmt);
    /* TODO I should really throttle this mtime checking to like every 50 ms or so */
    switch (Files.getMtime(cmt)) {
    | None => {Log.log("⚠️ cannot get docs for nonexistant cmt " ++ cmt); None}
    | Some(changed) =>
      if (changed > mtime) {
        newDocsForCmt(state.cmtCache, changed, cmt, src, state.clientNeedsPlainText);
      } else {
        Some(docs);
      }
    };
  } else {
    switch (Files.getMtime(cmt)) {
    | None => {Log.log("⚠️ cannot get docs for nonexistant cmt " ++ cmt); None}
    | Some(changed) => newDocsForCmt(state.cmtCache, changed, cmt, src, state.clientNeedsPlainText)
    };
  };

  }
};

let updateContents = (uri, text, version, state) => {
  Hashtbl.remove(state.compiledDocuments, uri);
  Hashtbl.replace(state.documentText, uri, (text, int_of_float(version), false));
  state
};

let getContents = (uri, state) => {
  let (text, _, _) = Hashtbl.find(state.documentText, uri);
  text
};

open Infix;
let getCompilationResult = (uri, state) => {
  if (Hashtbl.mem(state.compiledDocuments, uri)) {
    Hashtbl.find(state.compiledDocuments, uri)
  } else {
    let text = Hashtbl.mem(state.documentText, uri) ? {
      let (text, _, _) = Hashtbl.find(state.documentText, uri);
      text
    } : {
      let path = Utils.parseUri(uri) |! "not a uri";
      Files.readFileExn(path)
    };
    let result = AsYouType.process(text, ~cacheLocation=state.rootPath /+ "node_modules" /+ ".lsp", state.compilerPath, state.refmtPath, state.includeDirectories, state.compilationFlags);
    Hashtbl.replace(state.compiledDocuments, uri, result);
    switch (AsYouType.getResult(result)) {
    | None => ()
    | Some((_, data)) => Hashtbl.replace(state.lastDefinitions, uri, data)
    };
    result
  }
};

let getLastDefinitions = (uri, state) => switch (Hashtbl.find(state.lastDefinitions, uri)) {
| exception Not_found => None
| data => Some(data)
};

let getDefinitionData = (uri, state) => switch (getCompilationResult(uri, state)) {
| Success(_, _, data) | TypeError(_, _, data) => Some(data)
| _ => None
};

let docsForModule = (modname, state) =>
  Infix.(
    if (Hashtbl.mem(state.pathsForModule, modname)) {
      let (cmt, src) = Hashtbl.find(state.pathsForModule, modname);
      Log.log("FINDING " ++ cmt ++ " src " ++ (src |? ""));
      docsForCmt(cmt, src, state) |?>> d => (d, src)
    } else {
      Log.log("No path for module " ++ modname);
      None;
    }
  );

let maybeFound = Definition.maybeFound;

open Infix;

let topLocation = uri => {
        Location.loc_ghost: false,
        loc_start: {Lexing.pos_fname: uri, pos_lnum: 1, pos_cnum: 1, pos_bol: 1},
        loc_end: {Lexing.pos_fname: uri, pos_lnum: 1, pos_cnum: 1, pos_bol: 1},
      };

/* TODO instead of (option, option, option), it should be (option(docs), option((uri, loc))) */
let resolveDefinition = (uri, defn, state) =>
  switch defn {
  | `Local(_, loc, item, docs, _) => Some((Some(loc), docs, Some(uri)))
  | `Global(top, children, suffix) =>
    {
      switch (
        maybeFound(List.assoc(top), state.localModules)
        |?> (
          ((cmt, src)) => {
            let uri = Utils.toUri(src);
            maybeFound(Hashtbl.find(state.compiledDocuments), uri)
            |?> AsYouType.getResult
            |?>> ((defn) => (defn, uri))
          }
        )
      ) {
      | Some(((cmtInfos, data), uri)) =>
        if (children == []) {
          Some((Some(topLocation(uri)), data.toplevelDocs, Some(uri)))
        } else {
          Definition.resolveNamedPath(data, children, suffix) |?> (((_, loc, _, docs)) => Some((Some(loc), docs, Some(uri))))
        }
      | None =>
        maybeFound(Hashtbl.find(state.pathsForModule), top)
        |?> (
          ((cmt, src)) => {
            let uri = src |?>> Utils.toUri;
            if (children == []) {
              Some((uri |?>> topLocation, docsForCmt(cmt, src, state) |?> fst, uri))
            } else {
              docsForCmt(cmt, src, state)
              |?>> snd
              |?> Docs.findPath(children)
              |?>> (((name, loc, docs, _)) => (Some(loc), docs, uri))
            }
          }
        )
      };
    }
  };

let getResolvedDefinition = (uri, defn, data, state) => {
  Definition.findDefinition(defn, data) |?> x => resolveDefinition(uri, x, state)
};

let definitionForPos = (uri, pos, data, state) =>
  Definition.locationAtPos(pos, data)
  |?> (((_, _, defn)) => getResolvedDefinition(uri, defn, data, state));

let referencesForPos = (uri, pos, data, state) => {
  /* TODO handle cross-file stamps, e.g. the location isn't a stamp */
  Definition.stampAtPos(pos, data)
  |?> stamp => {
    let externals = (Definition.isStampExported(stamp, data) |?>> ((exportedName, suffixName)) => {
      let thisModName = FindFiles.getName(uri);
      optMap(((modname, (cmt, src))) => {
        if (modname == thisModName) {
          None
        } else {
          getDefinitionData(Utils.toUri(src), state) |?> data => {
            Definition.maybeFound(Hashtbl.find(data.Definition.externalReferences), thisModName) |?> uses => {
              let realUses = Utils.filterMap(((path, loc, suffix)) => {
                if (path == [exportedName] && suffix == suffixName) {
                  Some((`Read, Utils.endOfLocation(loc, String.length(suffixName |? exportedName))))
                } else {
                  None
                }
              }, uses);
              if (realUses == []) {
                None
              } else {
                Some((Utils.toUri(src), realUses))
              }
            }
          }
        }
      }, state.localModules)
    }) |? [];
    Definition.highlightsForStamp(stamp, data) |?>> positions => [(uri, positions), ...externals]
  }
};
