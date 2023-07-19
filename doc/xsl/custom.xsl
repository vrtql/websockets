<?xml version='1.0'?>
<!-- Common DocBook customization layer settings -->
<xsl:stylesheet
    xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
    xmlns:fo="http://www.w3.org/1999/XSL/Format"
    xmlns:d="http://docbook.org/ns/docbook"
    version="1.0">

<!-- ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^ -->
<!-- Utility                                                                  -->
<!-- vvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvv -->

<!-- For importing HTML into documents. For example, if you want to natively
     include an HTML file named code.html, you would simply do the following
     in your XML.

     <?htmlcode  include/code.html ?>

     Note that the HTML must be XHTML, so you may have to run it through
     htmltidy first to clean it up.
-->

<xsl:template match="processing-instruction('htmlcode')">
  <xsl:variable name="codefile" select="document(normalize-space(.),/)"/>
  <xsl:copy-of select="$codefile/*/node()"/>
</xsl:template>

<xsl:template match="d:sourcecode">

  <xsl:variable name="href" select="@href"/>

  <xsl:variable name="sourcefile">
    <xsl:value-of select="concat('../xml/include/', @href, '.html')"/>
  </xsl:variable>

  <xsl:message terminate="no">
    <xsl:value-of select="$sourcefile"/>
  </xsl:message>

  <xsl:variable name="codefile" select="document(normalize-space($sourcefile),/)"/>

  <xsl:call-template name="anchor"/>

  <xsl:element name="pre">
    <xsl:attribute name="class">programlisting</xsl:attribute>

    <xsl:copy-of select="$codefile/*/node()"/>
  </xsl:element>

</xsl:template>

<!-- ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^ -->
<!-- Additional Style / Formatting                                            -->
<!-- vvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvv -->

<xsl:template match="d:cr">
  <a>
    <xsl:attribute name="href">
      <xsl:text>https://vrtql.github.io/ws-code-doc/root/codebrowser/src/</xsl:text>
      <xsl:value-of select="@f" disable-output-escaping="yes"/>
      <xsl:text>.html#</xsl:text>
      <xsl:value-of select="text()" disable-output-escaping="yes"/>
    </xsl:attribute>
    <xsl:apply-templates/>
  </a>
</xsl:template>

<xsl:template match="rhtml">
  <a>
    <xsl:attribute name="href">
      <xsl:value-of select="@href" disable-output-escaping="yes"/>
    </xsl:attribute>
    <xsl:apply-templates/>
  </a>
</xsl:template>

<xsl:template match="large">
  <p class="large">
    <xsl:apply-templates/>
  </p>
</xsl:template>

<xsl:template match="medium">
  <p class="medium">
    <xsl:apply-templates/>
  </p>
</xsl:template>

<xsl:template match="small">
  <p class="small">
    <xsl:apply-templates/>
  </p>
</xsl:template>

<xsl:template match="strong">
  <b>
    <xsl:apply-templates/>
  </b>
</xsl:template>

<xsl:template match="verse">
  <p><pre class="verse">
      <xsl:apply-templates/>
  </pre></p>
</xsl:template>


<!-- For importing HTML into documents. For example, if you want to natively
     include an HTML file named code.html, you would simply do the following
     in your XML.

     <?htmlcode  include/code.html ?>

     Note that the HTML must be XHTML, so you may have to run it through
     htmltidy first to clean it up.
 -->

<xsl:template match="processing-instruction('htmlcode')">
  <xsl:variable name="codefile" select="document(normalize-space(.),/)"/>
  <xsl:copy-of select="$codefile/*/node()"/>
</xsl:template>

<xsl:template match="d:strong">
  <strong>
    <xsl:apply-templates/>
  </strong>
</xsl:template>

<xsl:template match="d:verse">
  <p><pre class="verse">
    <xsl:apply-templates/>
  </pre></p>
</xsl:template>

<!-- Not employed here. Ignore element. -->
<xsl:template match="d:rhtml">
  <xsl:apply-templates/>
</xsl:template>

<!-- <xsl:param name="suppress.navigation" select="1"/> -->

<xsl:param name="html.stylesheet" select="'screen.css docbook.css'"></xsl:param>

<xsl:param name="html.ext" select="'.html'"/>

<xsl:param name="use.id.as.filename" select="1"></xsl:param>

<xsl:param name="funcsynopsis.style" select="ansi"></xsl:param>

<xsl:param name="root.filename" select="root"></xsl:param>

<xsl:template name="user.header.navigation">
  <!--
      <xsl:comment>#include virtual="/nav/search.html"</xsl:comment>
      <xsl:text>&#10;</xsl:text>
  -->
</xsl:template>

</xsl:stylesheet>
