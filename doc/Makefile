.PHONY: all

HTML_DIR   = html
DOCBOOKXSL = xsl/docbook/xsl
CUSTOM_XSL_HTML_CHUNK=$(PWD)/xsl/html_chunk.xsl

all:	includes ws

includes: include/*
	./scripts/makeincludes.sh

ws: xml/* xml/ includes
	cd $(HTML_DIR) && xsltproc -o $@ -path $(DOCBOOKDTD) \
        --nonet                                          \
        --stringparam section.autolabel 1                \
        --stringparam chunker.output.encoding UTF-8      \
        --xinclude $(CUSTOM_XSL_HTML_CHUNK)              \
        $(PWD)/xml/$@.xml
