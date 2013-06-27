/*
 * File Handle class for JSyndicateFS
 */
package JSyndicateFS;

import JSyndicateFSJNI.struct.JSFSFileInfo;
import java.io.Closeable;
import java.io.IOException;
import org.apache.commons.logging.Log;
import org.apache.commons.logging.LogFactory;

/**
 *
 * @author iychoi
 */
public class FileHandle implements Closeable {

    public static final Log LOG = LogFactory.getLog(FileHandle.class);
    
    private static long currentIdGenerated = 1;
    
    private FileSystem filesystem;
    private FileStatus status;
    private boolean closed = true;
    private JSFSFileInfo fileinfo;
    private long id;
    
    private static long generateNewID() {
        currentIdGenerated++;
        return currentIdGenerated;
    }
    
    /*
     * Construct FileHandle from FileSystem and FileStatus
     */
    FileHandle(FileSystem fs, FileStatus status, JSFSFileInfo fileinfo) throws IOException {
        if(fs == null)
            throw new IllegalArgumentException("Can not create FileHandle from null filesystem");
        if(status == null)
            throw new IllegalArgumentException("Can not create FileHandle from null status");
        if(fileinfo == null)
            throw new IllegalArgumentException("Can not create FileHandle from null fileinfo");
        
        this.filesystem = fs;
        this.status = status;
        this.fileinfo = fileinfo;
        this.id = generateNewID();
        
        this.closed = false;
    }
    
    /*
     * Return FileInfo
     */
    JSFSFileInfo getFileInfo() {
        return this.fileinfo;
    }
    
    /*
     * Return handleID
     */
    public long getHandleID() {
        return this.id;
    }

    /*
     * Return FileSystem of the file
     */
    public FileSystem getFileSystem() {
        return this.filesystem;
    }
    
    /*
     * Return Path of the file
     */
    public Path getPath() {
        return this.status.getPath();
    }
    
    /*
     * Return FileStatus of the file
     */
    public FileStatus getStatus() {
        return this.status;
    }
    
    /*
     * Read data from the file
     */
    public int readFileData(byte[] buffer, int size, long offset) throws IOException {
        return this.filesystem.readFileData(this, buffer, size, offset);
    }
    
    /*
     * Write data to the file
     */
    public void writeFileData(byte[] buffer, int size, long offset) throws IOException {
        this.filesystem.writeFileData(this, buffer, size, offset);
    }
    
    /*
     * True if the file is open
     */
    public boolean isOpen() {
        if(this.status == null)
            return false;
        if(this.fileinfo == null)
            return false;
        
        return !this.closed;
    }
    
    @Override
    public void close() throws IOException {
        this.filesystem.closeFileHandle(this);
    }
    
    /*
     * This function will be called when handle is closed through other classes
     */
    void notifyClose() {
        this.fileinfo = null;
        this.closed = true;
    }
    
    /*
     * Return True if data is modified after loaded
     */
    public boolean isDirty() {
        if(this.status != null)
            return this.status.isDirty();
        return false;
    }
}