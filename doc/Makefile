.PHONY: all

HTML_DIR   = html
DOCBOOKXSL = xsl/docbook/xsl
CUSTOM_XSL_HTML_CHUNK=$(PWD)/xsl/html_chunk.xsl

all:	xml/doc.mod includes ws

# Generate the entity module the DocBook sources reference (ws.xml pulls in
# "doc.mod" as a parameter entity). The tracked template doc.mod.in needs no
# substitution, so a copy suffices. Without this a clean checkout fails the
# parse with "Entity 'mdash' not defined".
xml/doc.mod: xml/doc.mod.in
	cp $< $@

# Companion example programs, pinned to a known-good tag, fetched at build time
# so the walkthrough chapters include their real, compiling source (no drift, no
# vendoring). Network is required at doc-build (a deliberate tradeoff). Re-fetch
# by removing $(EXAMPLES_DIR).
EXAMPLES_REPO ?= https://github.com/vrtql/websockets-examples.git
EXAMPLES_TAG  ?= v0.1.0
EXAMPLES_DIR  ?= examples

$(EXAMPLES_DIR):
	git clone --depth 1 --branch $(EXAMPLES_TAG) $(EXAMPLES_REPO) $(EXAMPLES_DIR)

.PHONY: fetch-examples
fetch-examples: $(EXAMPLES_DIR)

includes: include/* fetch-examples
	./scripts/makeincludes.sh
	./scripts/makeincludes.sh $(EXAMPLES_DIR)/client
	./scripts/makeincludes.sh $(EXAMPLES_DIR)/server

ws: xml/doc.mod xml/* xml/ includes
	cd $(HTML_DIR) && xsltproc -o $@ -path $(DOCBOOKDTD) \
        --nonet                                          \
        --stringparam section.autolabel 1                \
        --stringparam chunker.output.encoding UTF-8      \
        --xinclude $(CUSTOM_XSL_HTML_CHUNK)              \
        $(PWD)/xml/$@.xml
