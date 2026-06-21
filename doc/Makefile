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

includes: include/*
	./scripts/makeincludes.sh

ws: xml/doc.mod xml/* xml/ includes
	cd $(HTML_DIR) && xsltproc -o $@ -path $(DOCBOOKDTD) \
        --nonet                                          \
        --stringparam section.autolabel 1                \
        --stringparam chunker.output.encoding UTF-8      \
        --xinclude $(CUSTOM_XSL_HTML_CHUNK)              \
        $(PWD)/xml/$@.xml
