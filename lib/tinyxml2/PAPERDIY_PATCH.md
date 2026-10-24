# Local TinyXML2 observer patch

PaperDIY adds an optional, per-document `SetProgressCallback(void (*)(void*), void*)` observer. `XMLNode::ParseDeep` calls it between nodes and `XMLElement::ParseAttributes` calls it between attributes. The reader gates physical input polling to approximately 50 ms in its callback.

The observer cannot abort, resume, mutate, or re-enter the parser. It changes no input bytes, node semantics, error result, or parsing order. The default null observer preserves ordinary TinyXML2 callers. A chapter still finishes before extraction cancellation or publication. A single token scan is not an interruptible operation.

The callback and context are instance fields; concurrent documents do not share them. Native and browser importers use this same source. Canonical text and serialized EPUB semantics are checked against the synchronous importer and restart fixtures. TinyXML2's upstream license remains in `LICENSE.txt`.
