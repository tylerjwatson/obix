/* *****************************************************************************
 * Copyright (c) 2014 Tyler Watson <tyler.watson@nextdc.com>
 * Copyright (c) 2013-2014 Qingtao Cao [harry.cao@nextdc.com]
 *
 * This file is part of oBIX.
 *
 * oBIX is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * oBIX is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with oBIX.  If not, see <http://www.gnu.org/licenses/>.
 *
 * *****************************************************************************/

#include <assert.h>
#include <libxml/tree.h>	/* xmlNode */
#include <libxml/xpath.h>
#include "xml_utils.h"		/* xpath_item_cb_t */
#include "obix_utils.h"
#include "log_utils.h"

const char *XML_HEADER = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n";
const char *XML_VERSION = "1.0";

/**
 * Apply the given callback on the given node and all its ancestors
 * all the way up to the topest level.
 *
 * Note,
 * 1. The callback function should check the xmlNode->type of the
 * current node and return if it is not desirable.
 */
int xml_for_each_ancestor_or_self(xmlNode *child,
								  xml_item_cb_t callback,
								  void *arg1, void *arg2)
{
	int ret;

	if (!child) {
		return 0;
	}

	if ((ret = callback(&child, arg1, arg2)) < 0) {
		return ret;
	}

	return xml_for_each_ancestor_or_self(child->parent, callback, arg1, arg2);
}

/**
 * Apply the specified callback function on every single node
 * with satisfactory type in the given subtree
 */
int xml_for_each_node_type(xmlNode *rootNode, xmlElementType type,
						   xml_item_cb_t callback, void *arg1, void *arg2)
{
	xmlNode *nextNode = NULL;
	int ret = 0;

	if (!rootNode) {
		return 0;
	}

	do {
		/*
		 * Save the next node's pointer in case the callback function
		 * may delete the current node
		 */
		nextNode = rootNode->next;

		/* If type equals to 0 then skip the comparison of it */
		if (type > 0 && rootNode->type != type) {
			continue;
		}

		/*
		 * IMPORTANT !!
		 *
		 * If the callback function deletes the given node, it MUST
		 * NULLIFY its pointer so as to avoid touching its descendant
		 */
		if ((ret = callback(&rootNode, arg1, arg2)) < 0) {
			break;
		}

		if (rootNode != NULL) {
			if ((ret = xml_for_each_node_type(rootNode->children, type,
											  callback, arg1, arg2)) < 0) {
				break;
			}
		}

	} while ((rootNode = nextNode) != NULL);

	return ret;
}

int xml_for_each_element(xmlNode *rootNode, xml_item_cb_t callback,
						 void *arg1, void *arg2)
{
	return xml_for_each_node_type(rootNode, XML_ELEMENT_NODE,
								  callback, arg1, arg2);
}

int xml_for_each_comment(xmlNode *rootNode, xml_item_cb_t callback,
						 void *arg1, void *arg2)
{
	return xml_for_each_node_type(rootNode, XML_COMMENT_NODE,
								  callback, arg1, arg2);
}

/**
 * Check if the given node has a postive hidden attribute
 */
int xml_is_hidden(const xmlNode *node)
{
	xmlChar *prop;
	int ret;

	if (!(prop = xmlGetProp((xmlNode *)node, BAD_CAST OBIX_ATTR_HIDDEN))) {
		return 0;
	}

	ret = xmlStrcmp(prop, BAD_CAST XML_TRUE);
	xmlFree(prop);

	return (ret == 0) ? 1 : 0;
}

/**
 * Re-enterant version of xml_copy.
 *
 * Note:
 * 1. recursionDepth keeps tracks of the number of times this function
 * recalls itself, it is used to return meta or hidden or comments
 * object if and only if they are explicitly requested, that is, when
 * recursionDepth equals to 1. Otherwise all such objects will be
 * skipped over according to the exludeFlags
 */
static xmlNode *xml_copy_r(const xmlNode *sourceNode,
						   xml_copy_exclude_flags_t excludeFlags,
						   int recursionDepth)
{
	xmlNode *nextNode = NULL;
	xmlNode *copyRoot = NULL;
	xmlNode *copyChild = NULL;

	if (!sourceNode) {
		return NULL;
	}

	if (recursionDepth > 0
		&& (excludeFlags & XML_COPY_EXCLUDE_HIDDEN) == XML_COPY_EXCLUDE_HIDDEN
		&& xml_is_hidden(sourceNode) == 1) {
		return NULL;
	}

	if (recursionDepth > 0
		&& (excludeFlags & XML_COPY_EXCLUDE_META) == XML_COPY_EXCLUDE_META
		&& xmlStrcmp(sourceNode->name, BAD_CAST OBIX_OBJ_META) == 0) {
		return NULL;
	}

	if (recursionDepth > 0
		&& (excludeFlags & XML_COPY_EXCLUDE_COMMENTS) == XML_COPY_EXCLUDE_COMMENTS
		&& sourceNode->type == XML_COMMENT_NODE) {
		return NULL;
	}

	/* Note:
	 * 2 to xmlCopyNode() means to copy node and all attributes,
	 * but no child elements.
	 */
	if ((copyRoot = xmlCopyNode((xmlNode *)sourceNode, 2)) == NULL) {
		log_error("Failed to copy the node");
		return NULL;
	}

	for (nextNode = sourceNode->children; nextNode; nextNode = nextNode->next) {
		if (nextNode->type != XML_ELEMENT_NODE ||
			!(copyChild = xml_copy_r(nextNode, excludeFlags, ++recursionDepth))) {
			continue;
		}

		if (xmlAddChild(copyRoot, copyChild) == NULL) {
			log_error("Failed to add the child copy into the current node");
			xmlFreeNode(copyChild);
		};
	}

	return copyRoot;
}

xmlNode *xml_copy(const xmlNode *sourceNode, xml_copy_exclude_flags_t excludeFlags)
{
	return xml_copy_r(sourceNode, excludeFlags, 0);
}

/**
 * Find in the specified DOM tree for a set of nodes that match the
 * given pattern, then invoke the provided callback function on each
 * of them
 */
void xml_xpath_for_each_item(xmlNode *rootNode, const char *pattern,
							 xpath_item_cb_t cb, void *arg1, void *arg2)
{
	xmlDoc *doc = NULL;
	xmlXPathObject *objects;
	xmlXPathContext *context;
	xmlNode *member;
	int i;

	assert(rootNode && pattern && cb);	/* parameters are optional */

	/*
	 * Create a temporary document for the standalone node
	 * that is not part of the global DOM tree
	 */
	if (!rootNode->doc) {
		if (!(doc = xmlNewDoc(BAD_CAST XML_VERSION))) {
			log_error("Failed to generate temp doc for XPath context");
			return;
		}

		xmlDocSetRootElement(doc, rootNode);
	}

	if (!(context = xmlXPathNewContext(rootNode->doc))) {
		log_warning("Failed to create a XPath context");
		goto ctx_failed;
	}

	/*
	 * If the provide node is not standalone but in the global DOM tree,
	 * have the search start from the current node instead of from
	 * the root of the global DOM tree
	 */
	if (!doc) {
		context->node = rootNode;
	}

	if (!(objects = xmlXPathEval(BAD_CAST pattern, context))) {
		log_warning("Unable to compile XPath expression: %s", pattern);
		goto xpath_failed;
	}

	/*
	 * Apply callback function on each matching node
	 */
	if (xmlXPathNodeSetIsEmpty(objects->nodesetval) == 0) {
		for (i = 0; i < xmlXPathNodeSetGetLength(objects->nodesetval); i++) {
			member = xmlXPathNodeSetItem(objects->nodesetval, i);
			/*
			 * Apply the callback function on each node tracked by
			 * the node set, and more importantly, nullify relevant
			 * pointer in the node set table just in case the callback
			 * function will release the pointed node. Since the whole
			 * node set will be released soon, this won't bring about
			 * any side effect at all.
			 *
			 * Or otherwise valgrind will detect invalid read at below
			 * address:
			 *
			 *		xmlXPathFreeNodeSet (xpath.c:4185)
			 *		xmlXPathFreeObject (xpath.c:5492)
			 *
			 * when it tries to access the xmlNode(its type member)
			 * that has been deleted!
			 */
			if (member != NULL) {
				cb(member, arg1, arg2);
				objects->nodesetval->nodeTab[i] = NULL;
			}
		}
	}

	/*
	 * If a temporary doc has been manipulated, unlink
	 * the original rootNode node from it so that the doc
	 * could be freed with no side effects on the rootNode
	 */
	if (doc) {
		xmlUnlinkNode(rootNode);
	}

	/* Fall through */

	xmlXPathFreeObject(objects);

xpath_failed:
	xmlXPathFreeContext(context);

ctx_failed:
	if (doc) {
		xmlFreeDoc(doc);
	}
}

/**
 * Helper function to find a direct child with matching tag
 * that has the specified attribute. If the val parameter is
 * not NULL, the value of that attribute must be the same
 * as val.
 *
 * If @a tag points to NULL, no comparison on the tag name is
 * performed (ie. it's ignored).
 *
 * Note,
 * 1. For sake of performance, oBIX server should strive to establish
 * a hierarchy organization of all XML objects, the global DOM tree
 * should strike a proper balance among its depth and width. If too
 * many direct children organized directly under one same parent,
 * this function will suffer hugh performance loss.
 */
xmlNode *xml_find_child(const xmlNode *parent, const char *tag,
						const char *attrName, const char *attrVal)
{
	xmlNode *node;
	xmlChar *attr_val;
	int ret;

	if (!parent) {
		return NULL;
	}

	assert(attrName);	/* attrVal, tag are optional */

	for (node = parent->children; node; node = node->next) {
		if (node->type != XML_ELEMENT_NODE) {
			continue;		/* only interested in normal nodes */
		}

		if (tag != NULL && xmlStrcmp(node->name, BAD_CAST tag) != 0) {
			continue;		/* tag not matching */
		}

		if (!(attr_val = xmlGetProp(node, BAD_CAST attrName))) {
			continue;		/* no specified attribute */
		}

		if (!attrVal) {			/* no need to compare attr's value */
			xmlFree(attr_val);
			return node;
		}

		ret = xmlStrcmp(attr_val, BAD_CAST attrVal);
		xmlFree(attr_val);

		if (ret == 0) {
			return node;	/* Found */
		}
	}

	return NULL;
}

/**
 * Get the value of the specified attribute of the given
 * node, and try to convert it to a long integer
 */
long xml_get_long(xmlNode *node, const char *attrName)
{
	xmlChar *attr_val;
	long val;

	if (!(attr_val = xmlGetProp(node, (xmlChar *)attrName))) {
		return -1;
	}

	val = str_to_long((const char *)attr_val);
	xmlFree(attr_val);

    return val;		/* Attribute value or error code */
}

/**
 * Get the value attribute of a matching children of the given node
 * with specific tag and name attribute.
 *
 * Note,
 * 1. Callers should release the returned string once done with it
 */
char *xml_get_child_val(const xmlNode *parent, const char *tag,
						const char *nameVal)
{
	xmlNode *node;

	if (!(node = xml_find_child(parent, tag, OBIX_ATTR_NAME, nameVal))) {
		return NULL;
	}

	return (char *)xmlGetProp(node, BAD_CAST OBIX_ATTR_VAL);
}

/**
 * Get the value attribute of a matching children of the given node
 * with specific tag and attribute, and try to convert it into a
 * long integer
 */
long xml_get_child_long(const xmlNode *parent, const char *tag,
						const char *nameVal)
{
	char *value;
	long l;

	if (!(value = xml_get_child_val(parent, tag, nameVal))) {
		return -1;
	}

	l = str_to_long(value);
	free(value);

    return l;		/* Attribute value or error code */
}

