/**
 * ====================================================================
 * Copyright (c) 2000-2001 CollabNet.  All rights reserved.
 *
 * This software is licensed as described in the file COPYING, which
 * you should have received as part of this distribution.  The terms
 * are also available at http://subversion.tigris.org/license-1.html.
 * If newer versions of this license are posted there, you may use a
 * newer version instead, at your option.
 *
 * This software consists of voluntary contributions made by many
 * individuals.  For exact contribution history, see the revision
 * history and logs, available at http://subversion.tigris.org/.
 * ====================================================================
 *
 */
import java.util.Vector;
import java.util.Hashtable;
import java.util.Date;
import org.tigris.subversion.lib.*;

/**
 * This classed is used for the unit tests. All of the C helper
 * functions for the Java Subversion binding should be reached
 * with this class. 
 *
 * Sometimes this is not possible, because
 * the class needs non-trivial native parameters. In this case
 * either simple type parameters are used or the methode
 * here designs a special case with no parameter
 */
public class NativeWrapper
{
    static
	{
	    System.loadLibrary("svn_jni_nativewrapper");
	}

    /**
     * Calls the function "vector__create" (vector.h)
     *
     * @return new, empty Vector instance
     */
    public static native Vector vectorCreate();

    /**
     * Calls the function "vector__add" (vector.h)
     *
     * @param vector instance of a vector that should be used for
     *               the operation
     * @param object
     */
    public static native void vectorAdd(Vector vector, Object object);

    /**
     * Create a new date from a long value.
     *
     * @param date milliseconds since Januar 1, 1970 00:00
     */
    public static native Date dateCreate(long date);

    /**
     * setter for "url" in Entry class
     */
    public static native void entrySetUrl(Entry entry, String url);

    /**
     * setter for "revision" in Entry class
     */
    public static native void entrySetRevision(Entry entry, long revision);
    
    /**
     * setter for "nodekind" in Entry class
     */
     public static native void entrySetNodekind(Entry entry, int nodekind);

    /**
     * setter for "schedule" in Entry class
     */
    public static native void entrySetSchedule(Entry entry, int schedule);
    
    /**
     * setter for "conflicted" in Entry class
     */
    public static native void entrySetConflicted(Entry entry, boolean conflicted);
       
    /**
     * setter for "copied" in Entry class
     */
    public static native void entrySetCopied(Entry entry, boolean copied);

    /**
     * setter for "texttime" in Entry class
     */
    public static native void entrySetTexttime(Entry entry, Date texttime);
    
    /**
     * setter for "proptime" in Entry class
     */
    public static native void entrySetProptime(Entry entry, Date proptime);

    /**
     * setter for "attributes" in Entry class
     */
    public static native void entrySetAttributes(Entry entry, Hashtable attributes);
}

/* 
 * local variables:
 * eval: (load-file "../../../../../../../svn-dev.el")
 * end: 
 */

